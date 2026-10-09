// sim.c - the fake PC for m6-stage2-run.sh. Linked with a COPY of the REAL tools/pc/navi48test.c in which only dr_call / open_service / main are replaced (build.sh):
// every verb the kit parses (m6stat, m6xstat, vramstat, disp2 crc|status|plane|show|fbhold) therefore prints through the REAL CLI printf bodies; this file only supplies the kernel's numbers.
// The world is a function of VIRTUAL time (SIM_DIR/vclock_ms), the restart epoch, the kill files and the scenario config (SIM_DIR/cfg/<key>, numbers). State that changes (held planes, restores) lives in SIM_DIR/kv/<key>.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdarg.h>
#include "../../src/navi48-bringup/src/dcn/navi48_disp2.h"
#include "../../src/navi48-bringup/src/dcn/navi48_dispread.h"
#define BASE_EPOCH 1791470000LL
static const char *SD, *FR;
static double cfg(const char *k, double d) { char p[512]; snprintf(p, sizeof p, "%s/cfg/%s", SD, k); FILE *f = fopen(p, "r"); if (!f) return d; double v = d; if (fscanf(f, "%lf", &v) != 1) v = d; fclose(f); return v; }
static long long kvg(const char *k, long long d) { char p[512]; snprintf(p, sizeof p, "%s/kv/%s", SD, k); FILE *f = fopen(p, "r"); if (!f) return d; long long v = d; if (fscanf(f, "%lld", &v) != 1) v = d; fclose(f); return v; }
static void kvs(const char *k, long long v) { char p[512]; snprintf(p, sizeof p, "%s/kv/%s", SD, k); FILE *f = fopen(p, "w"); if (f) { fprintf(f, "%lld\n", v); fclose(f); } }
static long long vms(void) { char p[512]; snprintf(p, sizeof p, "%s/vclock_ms", SD); FILE *f = fopen(p, "r"); long long v = 0; if (f) { if (fscanf(f, "%lld", &v) != 1) v = 0; fclose(f); } return v; }
static long long now_epoch(void) { return BASE_EPOCH + vms() / 1000; }
static int exists(const char *rel) { char p[512]; snprintf(p, sizeof p, "%s%s", FR, rel); struct stat sb; return stat(p, &sb) == 0; }
// ---- the geometry of the world
static const uint64_t A_[3] = { 0, 0x8004360000ull, 0x8002740000ull }, B_[3] = { 0, 0x8004b50000ull, 0x8003550000ull };
static const uint64_t SLB[3][3] = { {0,0,0}, {0x800bb40000ull, 0x800c340000ull, 0x800cb40000ull}, {0x8008e80000ull, 0x8009cc0000ull, 0x800ab00000ull} };
#define SL sl_tab()
static uint64_t SLX[3][3];
static double cfg(const char *k, double d);
static uint64_t (*sl_tab(void))[3] { memcpy(SLX, SLB, sizeof SLX); if (cfg("overlap_slot", 0) > 0) SLX[1][0] = SLB[2][1] + 0x100000; return SLX; }
static const uint64_t DPSL[3] = { 0x8005440000ull, 0x8006280000ull, 0x80070c0000ull };
#define DPCON 0x8001900000ull
static const unsigned SLOTMIB4[3] = { 0, 2381, 4219 };   // 3 * slot, in 1/100 MiB: monitor A 3*7.9375=23.81, monitor B 3*14.0625=42.19
// ---- per-instance timeline (ts = seconds since the WindowServer restart; < 0 before it)
struct ist { int enabled, acq, poolref, killed; long long acq_ts; };
static long long ts_now(void) { long long r = kvg("restart_epoch", -1); return r < 0 ? -1 : now_epoch() - r; }
// the pool arbitration, PURE: instances acquire in flip_at order; one needs 3 slots + 32 MiB of margin against what is free; free_at[i] = the free figure the instance saw at its verdict
static void arbitrate(int en[3], int acq[3], int ref[3], double free_at[3], double need[3]) {
    double big = cfg("pool_big", 0), fr = cfg("pool_free", big ? 900 : 97);
    long long t1 = (long long)cfg("flip_at_1", 13), t2 = (long long)cfg("flip_at_2", 11);
    en[1] = !(int)kvg("rst_kill1", 0) && (int)cfg("stage2b", 0); en[2] = !(int)kvg("rst_kill2", 0);
    int order[2]; if (t1 < t2) { order[0] = 1; order[1] = 2; } else { order[0] = 2; order[1] = 1; }
    for (int j = 0; j < 3; j++) { acq[j] = ref[j] = 0; free_at[j] = 0; need[j] = 0; }
    for (int q = 0; q < 2; q++) {
        int j = order[q]; if (!en[j]) continue;
        need[j] = SLOTMIB4[j] / 100.0 + 32.0; free_at[j] = fr;
        char kk[64]; snprintf(kk, sizeof kk, "poolref_%d", j);
        if (fr >= need[j] && !(int)cfg(kk, 0)) { acq[j] = 1; fr -= SLOTMIB4[j] / 100.0; } else ref[j] = 1;
    }
}
static void inst_state(int i, long long ts, struct ist *s) {
    memset(s, 0, sizeof *s);
    int en[3], acq[3], ref[3]; double fa[3], nd[3]; arbitrate(en, acq, ref, fa, nd);
    char k[64]; snprintf(k, sizeof k, "flip_at_%d", i);
    s->acq_ts = (long long)cfg(k, i == 2 ? 11 : 13);
    s->enabled = en[i];
    if (!en[i] || ts < s->acq_ts) return;
    if (ref[i]) { s->poolref = 1; return; }
    if (!acq[i]) return;
    // kill file: its first observation turns the instance off (one VERIFIED restore of A)
    const char *kf = i == 1 ? "/private/tmp/n48m-noflip1" : "/private/tmp/n48m-noflip2";
    char kn[64]; snprintf(kn, sizeof kn, "killed_ts_%d", i);
    if (exists(kf) && !(int)cfg(i == 1 ? "kill_ignore_1" : "kill_ignore_2", 0)) {
        long long kt = kvg(kn, -1);
        if (kt < 0 && getenv("SIM_NOKILLSIDE") == NULL) { kt = ts; kvs(kn, kt); kvs(i == 1 ? "restores_1" : "restores_2", kvg(i == 1 ? "restores_1" : "restores_2", 0) + 1); }
        s->killed = kt >= 0; if (s->killed) return;
    }
    s->acq = 1;
}
static long long restores_of(int i, long long ts, const struct ist *s) { long long r = kvg(i == 1 ? "restores_1" : "restores_2", 0); if (s->poolref && ts >= 0) r = r > 1 ? r : 1; return r; }
static double after_ts(const char *key, double d) { return cfg(key, d); }        // a time (s after the restart) at which a fault starts; 1e9 = never
// the acquired instance's content: it changes every 6 s for the first 30 s after the flip, then it is at rest
static int slot_idx(int i, long long ts, const struct ist *s) { long long a = ts - s->acq_ts; if (a < 0) a = 0; if (a > 30) a = 30; return (int)((a / 6 + i) % 3); }
static void crc_of(int i, long long ts, const struct ist *s, uint32_t *rg, uint32_t *b) {
    if (i == 1 && !s->acq && cfg("crc1_leave_at", 1e9) <= ts) { *rg = 0x5a5a0001u; *b = 0x5a5a0002u; return; }
    if (!s->acq) { *rg = i == 2 ? 0x78097625u : 0xe7a0b345u; *b = i == 2 ? 0x4bd14c15u : 0xb196d20fu; if (i == 1 && cfg("crc_a1_other", 0)) { *rg = 0x11112222u; *b = 0x33334444u; } return; }
    long long a = ts - s->acq_ts; if (a > 30) a = 30; if (a < 0) a = 0;
    *rg = 0x10000000u * (uint32_t)i + 0x1111u * (uint32_t)(a / 6 + 1); *b = 0x20000000u * (uint32_t)i + 0x77u * (uint32_t)(a / 6 + 1);
    if (cfg(i == 1 ? "monbcrc_a_at_1" : "monbcrc_a_at_2", 1e9) <= ts) { *rg = i == 2 ? 0x78097625u : 0xe7a0b345u; *b = i == 2 ? 0x4bd14c15u : 0xb196d20fu; }
}
static void addr_of(int i, long long ts, const struct ist *s, uint64_t *prog, uint64_t *early, int *pend) {
    *pend = 0;
    if (!s->acq) { *prog = *early = A_[i]; return; }
    int k = slot_idx(i, ts, s); *prog = *early = SL[i][k];
    if (cfg(i == 1 ? "fp_stuck_at_1" : "fp_stuck_at_2", 1e9) <= ts) { *pend = 1; *prog = SL[i][0]; *early = SL[i][1]; }
    double sw = cfg(i == 1 ? "swap_addr_at_1" : "swap_addr_at_2", 1e9);       // the plane address equals the OTHER display's slot
    if (sw <= ts) { int o = 3 - i; *prog = *early = SL[o][0]; }
    double outset = cfg(i == 1 ? "outside_at_1" : "outside_at_2", 1e9);       // outside {A, slots}
    if (outset <= ts) { *prog = *early = SL[i][2] + 0x10000; }
    double hi = cfg(i == 1 ? "hihigh_at_1" : "hihigh_at_2", 1e9);             // HIGH dword != A's
    if (hi <= ts) { *prog |= 0x100000000ull; *early |= 0x100000000ull; }
}
static unsigned uf_of(int i, long long ts) { return cfg(i == 1 ? "uf_at_1" : "uf_at_2", 1e9) <= ts ? 1u : 0u; }
static unsigned vmf_of(long long ts) { return cfg("vmf_at", 1e9) <= ts ? 0x1u : 0u; }
static int monbw_mask(int i, long long ts, const struct ist *s2) {      // the monitor A's crc op: the monitor B watch change mask
    if (i != 1) return 0;
    int m = 0;
    if (s2->acq && ts >= 0) { long long q = (now_epoch()) % 3; if (q == 0) m |= 0x40; }   // row 6 now and then: benign
    double at = cfg("monbw_row_at", 1e9); int row = (int)cfg("monbw_row", 4);
    if (at <= ts) m |= 1 << row;
    return m;
}
// ---- the encoders (the layouts are the CLI's printf decodes)
static void enc_m6xstat(int inst, unsigned page, uint64_t *v) {
    long long ts = ts_now(); struct ist s, s2; inst_state(inst, ts, &s); inst_state(3 - inst, ts, &s2);
    int latch1 = (int)cfg("latch1_off", 0) ? 0 : 1;
    uint64_t f = 3u | (latch1 ? (1u << 11) : 0u);
    if (inst == 1 && !latch1) f &= ~(1u << 11);
    unsigned acq = s.acq ? 1 : 0;
    if (acq) f |= 4u;
    uint32_t rg, bb; crc_of(inst, ts, &s, &rg, &bb); (void)rg; (void)bb;
    uint64_t prog, early; int pend; addr_of(inst, ts, &s, &prog, &early, &pend);
    if (acq && early != SL[inst][0] && early != SL[inst][1] && early != SL[inst][2] && early != A_[inst]) f |= 4u << 8;            // GPU-HELD code 4
    else if (acq) { int k = (early == SL[inst][0]) ? 1 : (early == SL[inst][1]) ? 2 : (early == SL[inst][2]) ? 3 : 0; f |= (uint64_t)k << 8; }
    if (cfg(inst == 1 ? "gh4_at_1" : "gh4_at_2", 1e9) <= ts && acq) { f &= ~(7ull << 8); f |= 4ull << 8; }
    long long rest = restores_of(inst, ts, &s);
    long long wd = cfg(inst == 1 ? "wdog_at_1" : "wdog_at_2", 1e9) <= ts ? 1 : 0;
    long long prs = 0; if (acq) { double rate = inst == 2 ? 30 : 5; long long a = ts - s.acq_ts; if (a < 0) a = 0; prs = (long long)(a * rate); double st = cfg("monbstall_at", 1e9); if (inst == 2 && st <= ts) prs = (long long)((st - s.acq_ts) * rate); }
    long long reuse = cfg(inst == 1 ? "reuse_at_1" : "reuse_at_2", 1e9) <= ts ? 2 : 0;
    long long wref = cfg(inst == 1 ? "wref_at_1" : "wref_at_2", 1e9) <= ts ? 1 : 0, wfail = cfg(inst == 1 ? "wfail_at_1" : "wfail_at_2", 1e9) <= ts ? 1 : 0, resfail = cfg(inst == 1 ? "resfail_at_1" : "resfail_at_2", 1e9) <= ts ? 1 : 0;
    long long gen = ts < 0 ? 0 : 8;
    for (int q = 0; q < 13; q++) v[q] = 0;
    v[0] = f; if (s.acq || s.killed || s.poolref) v[0] |= 64; if (cfg(inst == 1 ? "resfail_at_1" : "resfail_at_2", 1e9) <= ts) v[0] |= 32;
    if (page == 0) {
        if (s.acq || s.killed || s.poolref) v[0] |= 64;
        v[1] = (uint64_t)(s.acq || s.killed || s.poolref ? 1 : 0) | ((uint64_t)(s.acq || s.killed || s.poolref ? 1 : 0) << 32);
        if (s.acq || s.killed || s.poolref) { v[2] = A_[inst]; v[3] = B_[inst]; }
        v[4] = (uint64_t)prs | ((uint64_t)prs << 32); v[6] = (uint64_t)reuse << 32;
        v[7] = (uint64_t)wd | ((uint64_t)rest << 32); v[8] = (uint64_t)resfail; v[9] = (uint64_t)0 | ((uint64_t)(gen) << 32);
        if (cfg("tgen_other", 0)) v[9] = (uint64_t)0 | ((uint64_t)(gen + 5) << 32);
        v[10] = 1200; v[11] = 14000; v[12] = A_[inst];
    } else if (page == 1) {
        for (int k = 0; k < 3; k++) { v[1 + k] = s.acq ? SL[inst][k] : 0; v[4 + k] = (uint64_t)(s.acq ? 10 : 0) | ((uint64_t)(s.acq ? 10 : 0) << 32); }
        v[7] = (uint64_t)wref | ((uint64_t)wfail << 32); v[11] = 0xffffffffull | (0xffffffffull << 32); v[9] = 1; v[10] = 2;
    } else {
        uint32_t hc = (uint32_t)((inst == 1 ? 1u : 2u) << 4) | (uf_of(inst, ts) << 28) | 0x80000000u * 0; uint32_t vmf = vmf_of(ts);
        uint32_t odm = uf_of(inst, ts) ? (1u << 10) : 0u; long long frames = 100000 + now_epoch() * 60 - BASE_EPOCH * 60;
        if (cfg(inst == 1 ? "otgstuck_1" : "otgstuck_2", 0) && ts >= cfg(inst == 1 ? "otgstuck_at_1" : "otgstuck_at_2", 1e9)) frames = 99999;
        uint32_t fcr = pend ? 0x100u : 0u;
        v[1] = (uint64_t)hc | ((uint64_t)vmf << 32); v[2] = (uint64_t)odm | ((uint64_t)frames << 32);
        v[3] = (uint64_t)fcr | ((uint64_t)(inst == 1 ? 0x04380780u : 0x05a00a00u) << 32);
        v[4] = (prog & 0xFFFFFFFFull) | ((prog >> 32) << 32); v[5] = (early & 0xFFFFFFFFull) | ((early >> 32) << 32);
        v[6] = (uint64_t)(inst == 1 ? 0x77fu : 0x9ffu) | ((uint64_t)8u << 32); v[7] = 0; v[8] = 0x30000000u;
        v[9] = 6u | (0u << 8) | ((uint64_t)1 << 16); v[10] = A_[inst]; v[11] = B_[inst];
    }
}
static void enc_m6stat(unsigned page, uint64_t *v) {
    long long ts = ts_now(), now = vms() / 1000; struct ist s1, s2; inst_state(1, ts, &s1); inst_state(2, ts, &s2);
    int restarted = ts >= 0; long long tt = restarted ? ts : 0;
    double sr[3] = { cfg("srate0", 47), cfg("srate1", 3), cfg("srate2", 40) };
    double drop = cfg("srate0_drop_at", 1e9); if (restarted && drop <= ts) sr[0] = 0;
    double drop2 = cfg("srate2_drop_at", 1e9); if (restarted && drop2 <= ts) sr[2] = 0;
    for (int q = 0; q < 13; q++) v[q] = 0;
    v[0] = 0;
    int ids[3] = { 2, 3, (int)cfg("ids2", 3) }; if (!restarted) ids[0] = ids[1] = ids[2] = 0;
    uint64_t pipes = kvg("adopted", 0) ? 3 : 0;
    v[1] = 1u | (kvg("adopted", 0) ? 2u : 0u) | (kvg("armed", 0) ? 4u : 0u) | (pipes << 8) | ((uint64_t)(restarted ? 7 : 0) << 16);
    if (page == 0) {
        long long tx[3] = { restarted ? 500 + tt * 40 : 0, restarted ? 80 + tt * 2 : 0, restarted ? 200 + tt * 30 : 0 }; if (cfg("txn1_zero", 0)) tx[1] = 0;
        for (int i = 0; i < 3; i++) { v[2 + i] = tx[i]; v[5 + i] = ids[i]; }
        v[8] = (uint64_t)cfg("amb", 0); v[11] = (uint64_t)cfg("nofb", 0); v[12] = restarted ? 40000 + tt * 40 : 0;
    } else if (page == 1) {
        for (int i = 0; i < 3; i++) { v[2 + i] = (uint64_t)((restarted ? 300 : 0) + sr[i] * (double)now); v[5 + i] = (i == 1 ? (uint64_t)cfg("stamp_ref1", 0) : 0); v[8 + i] = 16666667; }
        if (!restarted) { for (int i = 0; i < 3; i++) v[2 + i] = 0; }
        long long nfb = restarted ? (long long)cfg("nfb", 3) : 0;
        v[12] = (uint64_t)nfb | ((uint64_t)(restarted ? (cfg("n921_1", 6) + cfg("n921_2", 6)) : 0) << 8) | ((uint64_t)(restarted ? 12 : 0) << 24) | ((uint64_t)(restarted ? 2 : 0) << 40);
    } else if (page == 4) {
        v[2] = restarted ? 0 : 0; v[3] = restarted ? (uint64_t)cfg("n921_1", 6) : 0; v[4] = restarted ? (uint64_t)cfg("n921_2", 6) : 0; v[5] = v[6] = v[7] = 0;
        v[8] = restarted ? (uint64_t)cfg("epmask", 7) : 0; v[9] = restarted ? (uint64_t)cfg("nfb", 3) : 0; v[10] = (uint64_t)cfg("epout", 0); v[11] = 2;
    } else if (page == 2) {
        for (int i = 0; i < 3; i++) for (int k = 0; k < 3; k++) v[2 + i * 3 + k] = k < ids[i] ? (uint64_t)(100 * (i + 1) + k + 1) : 0;
    } else {
        for (int i = 0; i < 3; i++) v[2 + i] = restarted ? 120 : ~0ull;
    }
}
static void enc_plane(uint32_t op, uint32_t inst, uint64_t *v) {
    long long ts = ts_now(); struct ist s, so; inst_state(inst, ts, &s); inst_state(3 - inst, ts, &so);
    struct n48d2_out o; memset(&o, 0, sizeof o); uint32_t g[N48D2_GATES]; memset(g, 0, sizeof g);
    o.op = op; o.fail = 0xFFu; o.status = N48D2_OK; o.inst = inst;
    char hk[32]; snprintf(hk, sizeof hk, "held_%u", inst); int held = (int)kvg(hk, 0);
    uint32_t refuses = (op == N48D2_OP_TIMING || op == N48D2_OP_CONNECT || op == N48D2_OP_OFF || op == N48D2_OP_TIMING1440 || op == N48D2_OP_PLANE || op == N48D2_OP_SHOW || op == N48D2_OP_FLIP || op == N48D2_OP_PLANEOFF || op == N48D2_OP_FBHOLD);
    uint64_t prog, early; int pend; addr_of(inst, ts, &s, &prog, &early, &pend);
    uint32_t rg, bb; crc_of(inst, ts, &s, &rg, &bb);
    if (held && refuses) { o.status = N48D2_HELD; }
    if (op == N48D2_OP_FBHOLD && o.status == N48D2_OK) kvs(hk, 1);
    o.f0a = 1000; o.f0b = 1003; o.f1a = 2000; o.f1b = 2003;
    g[N48D2_GT_HUBP_CNTL] = ((inst == 1 ? 1u : 2u) << 4) | (uf_of(inst, ts) << 28); g[N48D2_GT_HUBP_CLK] = 1;
    g[N48D2_GT_ODM2] = uf_of(inst, ts) ? (1u << 10) : 0; g[N48D2_GT_VMFAULT] = vmf_of(ts); g[N48D2_GT_DET2] = 3u << 8;
    g[N48D2_GT_EARLY_LO] = (uint32_t)early; g[N48D2_GT_EARLY_HI] = (uint32_t)(early >> 32);
    g[N48D2_GT_FLIP_CTL] = pend ? N48D2_FLIP_PENDING_MASK : 0; g[N48D2_GT_CRC_RG] = rg; g[N48D2_GT_CRC_B] = bb;
    g[N48D2_GT_A_LO] = (uint32_t)A_[inst]; g[N48D2_GT_A_HI] = (uint32_t)(A_[inst] >> 32); g[N48D2_GT_B_LO] = (uint32_t)B_[inst]; g[N48D2_GT_B_HI] = (uint32_t)(B_[inst] >> 32);
    g[N48D2_GT_FIFO] = (uint32_t)(0x1d | ((uint32_t)cfg("fifoerr", 0) << 28)); g[N48D2_GT_MPCC_STATUS] = 2;
    uint32_t meta = (held || op == N48D2_OP_FBHOLD ? N48D2_PL_HELD : N48D2_PL_SHOWN) & 0xFFu;
    int w2 = monbw_mask(inst, ts, &so);
    if (inst == 1 && w2 && (op == N48D2_OP_CRC)) { meta |= (uint32_t)w2 << 16; o.status = N48D2_DP_DISTURBED; for (int r = 0; r < 9; r++) if (w2 & (1 << r)) { o.guard = (uint32_t)r + 1; break; } }
    g[N48D2_GT_META] = meta;
    n48d2_pack_plane(v, &o, g);
}
static int hdr(uint64_t *out) { for (int i = 0; i < 16; i++) out[i] = 0; return 0; }
int sim_call(unsigned long long action, unsigned long long arg, unsigned long long *out) {
    SD = getenv("SIM_DIR"); FR = getenv("SIM_FR"); if (!SD || !FR) { fprintf(stderr, "sim: SIM_DIR / SIM_FR unset\n"); return -1; }
    hdr((uint64_t *)out); uint64_t *v = (uint64_t *)out + 3;
    if (action == 107) { unsigned page = (unsigned)(arg & 0xFF), inst = (unsigned)((arg >> 8) & 0xFF); if (inst == 0) inst = 2; enc_m6xstat((int)inst, page, v); return 0; }
    if (action == 106) { enc_m6stat((unsigned)arg, v); return 0; }
    if (action == 108) {
        long long ts = ts_now(); struct ist s1, s2; inst_state(1, ts, &s1); inst_state(2, ts, &s2);
        double big = cfg("pool_big", 0), free0 = cfg("pool_free", big ? 900 : 97); double fr = free0;
        if (ts >= 0) { if (s1.acq || s1.killed) fr -= SLOTMIB4[1] / 100.0; if (s2.acq || s2.killed) fr -= SLOTMIB4[2] / 100.0; }
        v[0] = 0; v[1] = (uint64_t)(big ? 985 : 217) << 20; v[2] = (uint64_t)(fr * 1048576.0); v[3] = 15280ull << 20; v[4] = 15000ull << 20; v[5] = (uint64_t)(big ? 1024 : 256) << 20; v[6] = (uint64_t)(big ? 1024 : 256) << 20;
        v[7] = 0xd0000000ull; v[8] = 16304ull << 20; v[9] = 0; v[10] = (uint64_t)(big ? 985 : 217) << 20; v[11] = 1; v[12] = 0x10000000ull; return 0; }
    if (action == N48DR_ACT_DISPCENSUS) {
        v[0] = (uint64_t)N48DR_OK; if (arg == N48DR_CENSUS4_PAGE) v[0] |= (uint64_t)N48DR_CENSUS4_COUNT << 16;
        else if (arg == N48DR_CENSUS6_PAGE) { v[0] |= (uint64_t)N48DR_CENSUS6_COUNT << 16; v[3] = DPCON; /* raw[4] = LOW, raw[5] = HIGH of HUBP0's primary */ }
        return 0; }
    if (action == N48D2_ACT) {
        uint32_t op = n48d2_arg_op(arg), inst = (uint32_t)((arg >> 8) & 0xFF); if (inst != 1 && inst != 2) inst = 1;
        if (op == N48D2_OP_STATUS) {
            uint32_t r[N48D2_STAT_COUNT]; memset(r, 0, sizeof r); long long n = vms();
            r[2] = (uint32_t)(n / 10); r[3] = r[2] + 3; r[21] = (uint32_t)(n / 10 + 7); r[22] = r[21] + 3; r[23] = 0x00000810u;
            if (ts_now() >= 0 && cfg("otg0_stop_at", 1e9) <= ts_now()) r[22] = r[21];
            v[0] = (uint64_t)N48D2_OK | ((uint64_t)op << 8) | ((uint64_t)N48D2_STAT_COUNT << 16);
            char hk[32]; snprintf(hk, sizeof hk, "held_%u", inst); if (kvg(hk, 0)) v[0] |= 1ull << N48D2_STAT_HELD_BIT;
            for (uint32_t i = 0; i < N48D2_STAT_COUNT; i++) v[1 + i / 2u] |= (uint64_t)r[i] << (32u * (i & 1u));
            return 0; }
        if (op == N48D2_OP_STATUS2 || op == N48D2_OP_STATUS3) { v[0] = (uint64_t)N48D2_OK | ((uint64_t)op << 8); return 0; }
        if (op == N48D2_OP_PLANEREC) { uint32_t rec[N48D2_REC_N]; memset(rec, 0, sizeof rec); n48d2_pack_rec(v, rec); return 0; }
        enc_plane(op, inst, v); return 0; }
    fprintf(stderr, "sim: unhandled action %llu arg %llu\n", action, arg); return -1;
}
// ---- the bundle's log, generated from the same world: `fakecli --genlog <t0> <t1>` prints the lines whose time lies in [t0, t1] (PC epochs)
static void ts_str(long long e, char *b, size_t n) { time_t t = (time_t)e; struct tm tm; localtime_r(&t, &tm); size_t k = strftime(b, n, "%Y-%m-%d %H:%M:%S", &tm); snprintf(b + k, n - k, ".%03d", 123); }
static void L(long long e, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static long long G0, G1; static int pidw;
static void L(long long e, const char *fmt, ...) {
    if (e < G0 || e > G1) return;
    char tb[64]; ts_str(e, tb, sizeof tb); char m[1400]; va_list ap; va_start(ap, fmt); vsnprintf(m, sizeof m, fmt, ap); va_end(ap);
    printf("%s Df WindowServer[%d:2a1b] (Navi48Metal) Navi48Metal: %s\n", tb, pidw, m);
}
int gen_log(long long t0, long long t1) {
    SD = getenv("SIM_DIR"); FR = getenv("SIM_FR"); G0 = t0; G1 = t1;
    long long r = kvg("restart_epoch", -1); if (r < 0) return 0;
    pidw = (int)kvg("ws_pid_after", 200);
    long long ts_end = now_epoch() - r;
    int en[3], acq[3], ref[3]; double fa[3], nd[3]; arbitrate(en, acq, ref, fa, nd);
    for (int i = 1; i <= 2; i++) {
        int rk = (int)kvg(i == 1 ? "rst_kill1" : "rst_kill2", 0), fe = (int)cfg("force_init_enabled", 0);
        L(r + 1, "scanout: instance %d (the %s): %s (kernel latches m6 1 m6flip 1 m6flip1 1, exports call 1 bo_handle 1, kill file %s; geometry %ux%u pitch %u B from the descriptor, slot %llu B)", i, i == 1 ? "monitor A" : "monitor B", (en[i] || fe) ? "ENABLED (acquired lazily at the first frame the table maps to it)" : "OFF", (rk && !fe) ? "PRESENT" : "absent", i == 1 ? 1920u : 2560u, i == 1 ? 1080u : 1440u, i == 1 ? 7680u : 10240u, i == 1 ? 8323072ull : 14745600ull);
    }
    L(r + 2, "scanout: query: 2560x1440 plane, pitch 2560 px, hubp_format 8 sw_mode 0, refresh 60000 mHz, flags 0x3 (LIT 1 NATIVE 1 ACQUIRED 0 GEOM_OK 1), console MC 0x%llx", (unsigned long long)DPCON);
    for (int k = 0; k < 3; k++) L(r + 3, "scanout: slot %d: memory type 1, 14745600 B (buffer req 14745600 align 65536), kernel slot id %d, MC 0x%llx (64 KiB aligned: 1)", k, k, (unsigned long long)DPSL[k]);
    for (int i = 1; i <= 2; i++) {
        if (!en[i]) continue;
        char kn[64]; snprintf(kn, sizeof kn, "flip_at_%d", i); long long at = (long long)cfg(kn, i == 2 ? 11 : 13); const char *nm = i == 1 ? "monitor A" : "monitor B";
        struct ist cur; inst_state(i, ts_end, &cur);
        if (acq[i]) {
            L(r + at, "scanout (instance %d, %s): free visible VRAM %d MiB covers this display's 3 slots + margin (need %d MiB)", i, nm, (int)fa[i], (int)nd[i]);
            for (int k = 0; k < 3; k++) L(r + at, "scanout (instance %d, %s): slot %d: %llu B, kernel slot id %#x, MC 0x%llx (64 KiB aligned: %d)", i, nm, k, i == 1 ? 8323072ull : 14745600ull, (unsigned)(i * 16 + k), (unsigned long long)SL[i][k], 1);
            L(r + at, "scanout (instance %d, %s) ACQUIRED, 3 slots (console A 0x%llx, frame count 1234, M6 table generation 8)", i, nm, (unsigned long long)A_[i]);
            double gw = cfg(i == 1 ? "geomword_at_1" : "geomword_at_2", 1e9);
            if (gw < 1e8) L(r + (long long)gw, "scanout (instance %d, %s): the kernel's plane geometry word 0x4380780 (5 output words) is NOT %ux%u pitch %u B: instance OFF", i, nm, i == 1 ? 1920u : 2560u, i == 1 ? 1080u : 1440u, i == 1 ? 7680u : 10240u);
            long long kt = cur.killed ? kvg(i == 1 ? "killed_ts_1" : "killed_ts_2", 1000000) : 1000000;
            for (long long q = at + 5; q <= ts_end && q < kt; q += 10) {
                long long a = q - at, pr = (long long)(a * (i == 2 ? 30.0 : 5.0)), gm = 0; double rise = cfg(i == 1 ? "georise_at_1" : "georise_at_2", 1e9);
                if (rise < 1e8 && q >= (long long)rise) gm = (q - (long long)rise) / 10 + 1;
                double st = cfg("monbstall_at", 1e9); if (i == 2 && st < 1e8 && q >= (long long)st) pr = (long long)((st - at) * 30.0);
                long long cached = cfg("tgen_other", 0) ? 7 : 8;
                L(r + q, "scanout (instance %d, %s): counters: presents %lld (kernel presents %lld latched %lld replaced 0 refused 0 watchdog_restores 0) drops 0 gpu_fail 0 stale_skipped 0 superseded 0; planned %lld inst_mismatch %d inst_geom_mismatch %lld stale_retry 0 stale_drop 0 tentative 0 re-copy noted 0 done 0 failed 0; status calls 9 (keep-alive 2); table gen 8 (cached %lld, refused out-of-order 0)", i, nm, pr, pr, pr, pr, (i == 2 ? 3 : 0), gm, cached);
            }
            if (cur.killed) {
                L(r + kt, "scanout (instance %d, %s): kill switch %s exists: this instance is being released (the kernel puts A back)", i, nm, i == 1 ? "/private/tmp/n48m-noflip1" : "/private/tmp/n48m-noflip2");
                L(r + kt, "scanout (instance %d, %s): release rc 0, restore of A verified 1, plane MC after 0x%llx", i, nm, (unsigned long long)A_[i]);
            }
        } else {
            L(r + at, "scanout (instance %d, %s): the free visible VRAM (%d MiB, the kernel's figure) does not cover this display's 3 slots + the 32 MiB margin (need %d MiB: verdict 2)", i, nm, (int)fa[i], (int)nd[i]);
            L(r + at, "scanout (instance %d, %s): FAIL CLOSED / OFF (slot-alloc): pool", i, nm);
        }
    }
    for (long long q = 3; q <= ts_end; q += 10) {
        long long dp = q * (long long)cfg("dp_present_rate", 47); double dd = cfg("srate0_drop_at", 1e9); if (dd < 1e8 && q >= (long long)dd) dp = (long long)dd * 47;
        L(r + q, "m6: surfaces for instance 1/2: 3/3 (kernel table 6 IDs: instance 0 2, ambiguous 0; refreshes %lld failed 0); frames completed without a present: at the gate instance 1 0 instance 2 0 ambiguous 0, at the present instance 1 0 instance 2 3 ambiguous 0 unknown 3; tentative at the gate 0; presented to instance 0 %lld", q, dp);
    }
    return 0;
}
int real_main(int, char **);
int main(int argc, char **argv) {
    if (argc >= 4 && !strcmp(argv[1], "--genlog")) return gen_log(atoll(argv[2]), atoll(argv[3]));
    return real_main(argc, argv);
}
