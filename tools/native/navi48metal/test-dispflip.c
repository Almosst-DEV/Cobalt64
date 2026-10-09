// test-dispflip.c: the S5.2a D-copy state machine (n48_dispflip.h) on the host: slot choice, drops, present-after-success, fail-closed transitions, kill switch.
// Build/run on the host Mac:  cc -O1 -Wall -Wextra -Werror -o /tmp/test-dispflip test-dispflip.c && /tmp/test-dispflip
#include <stdio.h>
#include <stdlib.h>
#include "n48_scanabi.h"
#include "n48_dispflip.h"
#include "n48_impcache.h"
static int fails;
#define CHECK(name, cond, ...) do { int ok_ = (cond); printf("%s %s: ", ok_ ? "ok  " : "FAIL", name); printf(__VA_ARGS__); printf("\n"); if (!ok_) fails++; } while (0)
#define R N48DF_SLOT_REUSABLE
static n48df_t active(void) { n48df_t s; n48df_init(&s, 0); n48df_mark_acquired(&s); n48df_activate(&s, 3); return s; }

// ---- native #12 P3 present hold: a simulated clock; submissions land at their own times, a wait advances the clock and lets the submissions due by then happen ----
#define MS 1000000ull
typedef struct { n48df_t *s; uint64_t clock; struct { uint64_t t, seq; uint32_t sid; int slot, done; } sub[4]; int nsub; uint64_t waited_sum, waited_max; int nwait; } hsim_t;
static void hsim_arrive(hsim_t *h, uint64_t upto) { for (int i = 0; i < h->nsub; i++) if (!h->sub[i].done && h->sub[i].t <= upto) { h->sub[i].done = 1; n48df_submit(h->s, h->sub[i].slot, h->sub[i].sid, h->sub[i].seq); } }
static void hsim_wait(void *c, uint64_t ns) { hsim_t *h = (hsim_t *)c; h->clock += ns; h->nwait++; h->waited_sum += ns; if (ns > h->waited_max) h->waited_max = ns; hsim_arrive(h, h->clock); }
// CoreDisplay's frame: G (seq 1, commit 0, GPU done 1.5 ms) then optionally C (seq 2, commit/submit at tc, GPU done 1 ms after). Returns bit0 = G presented, bit1 = C presented.
static int hold_frame(int on, int hms, int with_c, uint64_t tc, hsim_t *h, n48df_t *s) {
    *s = active(); memset(h, 0, sizeof *h); h->s = s;
    uint32_t all[3] = { R, R, R };
    int sg = n48df_pick(s, all), sc = with_c ? n48df_pick(s, all) : -1;
    n48df_submit(s, sg, 7, 1);
    if (with_c) { h->sub[h->nsub].slot = sc; h->sub[h->nsub].t = tc; h->sub[h->nsub].seq = 2; h->sub[h->nsub].sid = 7; h->nsub++; }
    int res = 0; uint64_t w;
    h->clock = 3 * MS / 2; hsim_arrive(h, h->clock);
    if (n48df_complete_held(s, sg, 0, 1, 7, on, hms, 0 + 1, h->clock, hsim_wait, h, &w)) { res |= 1; n48df_present_result(s, 0); }   // tcommit = 1 ns (0 means "unknown")
    if (with_c) {
        if (h->clock < tc + MS) h->clock = tc + MS; hsim_arrive(h, h->clock);
        if (n48df_complete_held(s, sc, 0, 2, 7, on, hms, tc, h->clock, hsim_wait, h, &w)) { res |= 2; n48df_present_result(s, 0); }
    }
    return res;
}
int main(void) {
    uint32_t all[3] = { R, R, R }, none[3] = { 0, 0, 0 };
    CHECK("abi flag", N48DF_SLOT_REUSABLE == N48N_SCANSLOT_REUSABLE && N48DF_MAX_SLOTS == (int)N48N_SCAN_MAX_SLOTS, "REUSABLE 0x%x == N48N_SCANSLOT_REUSABLE, 3 slots", N48DF_SLOT_REUSABLE);

    // ---- kill switch ----
    n48df_t k; n48df_init(&k, 1);
    CHECK("kill: off", k.state == N48DF_OFF && k.reason == N48DF_R_KILL, "state %d reason %d", k.state, k.reason);
    CHECK("kill: never activates", !n48df_activate(&k, 3) && k.state == N48DF_OFF, "refused");
    CHECK("kill: pick -1 and no drop", n48df_pick(&k, all) == -1 && k.drops == 0, "drops %llu", (unsigned long long)k.drops);
    CHECK("kill: nothing to release", n48df_fail(&k, N48DF_R_ERROR) == 0, "plane was never taken");
    CHECK("off-file bypass table", n48df_off_file_ignored(0, "1", "1", 0) && !n48df_off_file_ignored(0, "1", "1", 1) && !n48df_off_file_ignored(501, "1", "1", 0) &&
          !n48df_off_file_ignored(0, NULL, "1", 0) && !n48df_off_file_ignored(0, "1", NULL, 0) && !n48df_off_file_ignored(0, "0", "1", 0) && !n48df_off_file_ignored(0, "1", "0", 0) && !n48df_off_file_ignored(0, "1", "", 0),
          "ignored only for root + N48M_ALLOW=1 + N48M_TEST_IGNORE_KILL=1 and never for WindowServer");

    // ---- unbound ----
    n48df_t u; n48df_init(&u, 0);
    CHECK("unbound: pick -1 and no drop", u.state == N48DF_UNBOUND && n48df_pick(&u, all) == -1 && u.drops == 0, "state %d", u.state);
    CHECK("activate bounds", !n48df_activate(&u, 0) && !n48df_activate(&u, 4) && u.state == N48DF_UNBOUND && n48df_activate(&u, 3) && u.state == N48DF_ACTIVE && u.acquired, "0 and 4 slots refused, 3 accepted");
    CHECK("activate once", !n48df_activate(&u, 3), "second activate refused");

    // ---- slot choice: only REUSABLE and not in flight ----
    n48df_t s = active();
    int a = n48df_pick(&s, all), b = n48df_pick(&s, all), c = n48df_pick(&s, all);
    CHECK("three picks give three distinct slots", a >= 0 && b >= 0 && c >= 0 && a != b && b != c && a != c && n48df_inflight_count(&s) == 3, "%d %d %d", a, b, c);
    CHECK("all in flight: drop", n48df_pick(&s, all) == -1 && s.drops == 1, "drops %llu (status said REUSABLE for all: in-flight wins)", (unsigned long long)s.drops);
    CHECK("complete frees the slot", n48df_complete(&s, b, 0) == 1 && s.inflight[b] == 0, "present requested for slot %d", b);
    CHECK("freed slot is picked again", n48df_pick(&s, all) == b, "slot %d", b);
    n48df_complete(&s, a, 0); n48df_complete(&s, b, 0); n48df_complete(&s, c, 0);
    uint32_t only2[3] = { 0, 0, R };
    CHECK("only the REUSABLE slot", n48df_pick(&s, only2) == 2, "slot 2");
    n48df_complete(&s, 2, 0);
    uint32_t busy[3] = { N48N_SCANSLOT_PENDING | N48N_SCANSLOT_INUSE, N48N_SCANSLOT_PENDING, N48N_SCANSLOT_INUSE };
    CHECK("PENDING / INUSE are never chosen", n48df_pick(&s, busy) == -1, "dropped");
    CHECK("no REUSABLE flag at all: drop counted", n48df_pick(&s, none) == -1 && s.drops == 3, "drops %llu", (unsigned long long)s.drops);
    CHECK("a REUSABLE bit among others is enough", n48df_pick(&s, (uint32_t[3]){ N48N_SCANSLOT_PENDING, N48N_SCANSLOT_PENDING, R | 0x8 }) == 2, "slot 2");
    n48df_complete(&s, 2, 0);
    // rotation: successive picks with everything free walk the slots, so the slot just presented is not the next to be written
    n48df_t r = active(); int seq[6];
    for (int i = 0; i < 6; i++) { seq[i] = n48df_pick(&r, all); n48df_complete(&r, seq[i], 0); }
    CHECK("rotation", seq[0] == 0 && seq[1] == 1 && seq[2] == 2 && seq[3] == 0 && seq[4] == 1 && seq[5] == 2, "%d %d %d %d %d %d", seq[0], seq[1], seq[2], seq[3], seq[4], seq[5]);

    // ---- present only after success ----
    n48df_t p = active(); int sl = n48df_pick(&p, all);
    CHECK("pick does not present", p.presents == 0 && p.inflight[sl], "slot %d in flight, 0 presents", sl);
    CHECK("fence VK_SUCCESS -> present requested", n48df_complete(&p, sl, 0) == 1 && p.inflight[sl] == 0, "yes");
    CHECK("present ok counted", n48df_present_result(&p, 0) == 0 && p.presents == 1 && p.state == N48DF_ACTIVE, "presents %llu", (unsigned long long)p.presents);
    sl = n48df_pick(&p, all);
    CHECK("fence error -> NO present, slot freed", n48df_complete(&p, sl, -4) == 0 && p.inflight[sl] == 0 && p.gpu_fail == 1 && p.presents == 1 && p.state == N48DF_ACTIVE, "DEVICE_LOST style error: gpu_fail %llu, still ACTIVE (the kernel watchdog restores the console)", (unsigned long long)p.gpu_fail);
    CHECK("double complete is a no-op", n48df_complete(&p, sl, 0) == 0 && n48df_complete(&p, -1, 0) == 0 && n48df_complete(&p, 3, 0) == 0 && n48df_complete(&p, 99, 0) == 0, "no present for a slot that is not in flight or out of range");

    // ---- fail closed ----
    n48df_t f = active(); sl = n48df_pick(&f, all);
    int ok = n48df_complete(&f, sl, 0);
    CHECK("present error -> fail closed + release requested once", ok == 1 && n48df_present_result(&f, -16) == 1 && f.state == N48DF_OFF && f.reason == N48DF_R_ERROR && f.present_fail == 1 && f.released, "state OFF reason %d", f.reason);
    CHECK("after fail: second fail does not release again", n48df_fail(&f, N48DF_R_ERROR) == 0, "idempotent");
    CHECK("after fail: pick -1, no drop counted", n48df_pick(&f, all) == -1 && f.drops == 0, "no new copies");
    CHECK("after fail: activate refused", !n48df_activate(&f, 3), "OFF is final");
    n48df_t g = active(); int s0 = n48df_pick(&g, all), s1 = n48df_pick(&g, all);
    CHECK("status error in between: fail closed", n48df_fail(&g, N48DF_R_PLANELOST) == 1 && g.state == N48DF_OFF && g.reason == N48DF_R_PLANELOST, "plane lost");
    CHECK("in-flight copy completing after OFF: slot freed, NO present", n48df_complete(&g, s0, 0) == 0 && g.inflight[s0] == 0 && n48df_complete(&g, s1, 0) == 0 && n48df_inflight_count(&g) == 0, "nothing presented once closed");
    n48df_t h; n48df_init(&h, 0); n48df_mark_acquired(&h);
    CHECK("acquire done, a later step fails (never activated): release requested once", n48df_fail(&h, N48DF_R_ALLOC) == 1 && n48df_fail(&h, N48DF_R_ALLOC) == 0 && h.state == N48DF_OFF, "alloc failure path");
    n48df_t m; n48df_init(&m, 0);
    CHECK("export missing before acquire: OFF, no release", n48df_fail(&m, N48DF_R_NOSYM) == 0 && m.state == N48DF_OFF && m.reason == N48DF_R_NOSYM && m.errors == 0, "kernel v1 copy continues");
    n48df_t n; n48df_init(&n, 0);
    CHECK("ENOSYS (M4 path): OFF, no release", n48df_fail(&n, N48DF_R_NOSYS) == 0 && n.state == N48DF_OFF && n.reason == N48DF_R_NOSYS, "clean refusal");
    n48df_t q = active();
    CHECK("release-on-request path (fail reason 0) releases once", n48df_fail(&q, 0) == 1 && q.state == N48DF_OFF && q.errors == 0, "no error counted");

    // ---- #12: frame order (monotonic sequence at submission) ----
    {
        n48df_t o = active(); int a0 = n48df_pick(&o, all), a1 = n48df_pick(&o, all), a2 = n48df_pick(&o, all);   // submitted as seq 1, 2, 3
        CHECK("order: newest completes first -> presented", n48df_complete_seq(&o, a2, 0, 3) == 1 && o.last_seq == 3, "seq 3 presented");
        CHECK("order: an older frame completing later is NOT presented", n48df_complete_seq(&o, a0, 0, 1) == 0 && o.stale == 1 && o.inflight[a0] == 0, "seq 1 stale-skipped, slot released");
        CHECK("order: second older frame also skipped", n48df_complete_seq(&o, a1, 0, 2) == 0 && o.stale == 2 && o.presents == 0 && o.last_seq == 3, "stale %llu", (unsigned long long)o.stale);
        int b = n48df_pick(&o, all);
        CHECK("order: a stale slot is reusable at once", b >= 0 && n48df_complete_seq(&o, b, 0, 4) == 1 && o.last_seq == 4, "seq 4 presented");
        int c = n48df_pick(&o, all);
        CHECK("order: the same seq twice is not presented twice", c >= 0 && n48df_complete_seq(&o, c, 0, 4) == 0 && o.stale == 3, "equal seq is stale");
        int d = n48df_pick(&o, all);
        CHECK("order: a failed fence does not advance last_seq", d >= 0 && n48df_complete_seq(&o, d, -4, 9) == 0 && o.last_seq == 4 && o.stale == 3 && o.gpu_fail == 1, "last_seq 4");
        n48df_t q2 = active(); int e = n48df_pick(&q2, all); n48df_fail(&q2, N48DF_R_PLANELOST);
        CHECK("order: OFF presents nothing and counts no stale", n48df_complete_seq(&q2, e, 0, 7) == 0 && q2.stale == 0, "closed");
        // randomized: frames submitted in seq order, completions in any order (3 in flight); the presented sequence must be strictly increasing
        srand(777); n48df_t r2 = active(); int held2[3] = { 0, 0, 0 }; uint64_t sq[3] = { 0, 0, 0 }, next = 0, lastp = 0, viol = 0, npres = 0;
        for (int i = 0; i < 100000; i++) {
            int pk = n48df_pick(&r2, all); if (pk >= 0) { held2[pk] = 1; sq[pk] = ++next; }
            int cs = rand() % 3;
            if (held2[cs] && (rand() & 1)) { int w = n48df_complete_seq(&r2, cs, 0, sq[cs]); held2[cs] = 0; if (w) { npres++; n48df_present_result(&r2, 0); if (sq[cs] <= lastp) viol++; lastp = sq[cs]; } }
        }
        CHECK("order: 100000 out-of-order completions never present an older frame", viol == 0 && npres == r2.presents && r2.stale > 0, "%llu violations, %llu presented, %llu stale-skipped", (unsigned long long)viol, (unsigned long long)npres, (unsigned long long)r2.stale);
    }

    // ---- #12: superseded per-surface present skip ----
    {
        n48df_t o = active(); int a0 = n48df_pick(&o, all), a1 = n48df_pick(&o, all);      // seq 1 and 2 for surface 77, both with a slot
        n48df_submit(&o, a0, 77, 1); n48df_submit(&o, a1, 77, 2);
        CHECK("superseded: the earlier frame is skipped", n48df_complete_surf(&o, a0, 0, 1, 77) == 0 && o.superseded == 1 && o.stale == 0 && o.inflight[a0] == 0 && o.last_seq == 0, "seq 1 skipped, slot released, last_seq unchanged");
        CHECK("superseded: the later frame presents", n48df_complete_surf(&o, a1, 0, 2, 77) == 1 && o.last_seq == 2, "seq 2 presented");
        // the later one is the highest: it is not superseded by itself; a different surface is independent
        int b0 = n48df_pick(&o, all), b1 = n48df_pick(&o, all); n48df_submit(&o, b0, 5, 3); n48df_submit(&o, b1, 77, 4);
        CHECK("superseded: another surface is independent", n48df_complete_surf(&o, b0, 0, 3, 5) == 1 && n48df_complete_surf(&o, b1, 0, 4, 77) == 1 && o.superseded == 1, "both present");
        // later DROPPED (no slot): never submitted with a slot -> the earlier frame is NOT skipped
        n48df_t d = active(); int c0 = n48df_pick(&d, all); n48df_submit(&d, c0, 9, 10);       // seq 10 has a slot; seq 11 had none: no submit() call for it
        CHECK("later dropped: the earlier frame still presents", n48df_complete_surf(&d, c0, 0, 10, 9) == 1 && d.superseded == 0, "no skip when the later cb had no slot");
        // later FAILED (GPU error): the frame that failed is forgotten, so a still-pending earlier frame presents
        n48df_t f2 = active(); int e0 = n48df_pick(&f2, all), e1 = n48df_pick(&f2, all); n48df_submit(&f2, e0, 3, 20); n48df_submit(&f2, e1, 3, 21);
        CHECK("later failed first: gpu_fail counted, nothing presented", n48df_complete_surf(&f2, e1, -4, 21, 3) == 0 && f2.gpu_fail == 1 && f2.superseded == 0, "seq 21 failed");
        CHECK("later failed: the pending earlier frame presents", n48df_complete_surf(&f2, e0, 0, 20, 3) == 1 && f2.last_seq == 20 && f2.superseded == 0, "seq 20 presented (hi forgotten)");
        // later failed AFTER the earlier was already skipped: documented -> nothing shown until the next frame; the next frame presents
        n48df_t f3 = active(); int g0 = n48df_pick(&f3, all), g1 = n48df_pick(&f3, all); n48df_submit(&f3, g0, 3, 30); n48df_submit(&f3, g1, 3, 31);
        n48df_complete_surf(&f3, g0, 0, 30, 3); n48df_complete_surf(&f3, g1, -4, 31, 3);
        int g2 = n48df_pick(&f3, all); n48df_submit(&f3, g2, 3, 32);
        CHECK("later failed after skip: the next frame presents", f3.superseded == 1 && f3.presents == 0 && n48df_complete_surf(&f3, g2, 0, 32, 3) == 1 && f3.last_seq == 32, "skipped 30, failed 31, 32 presents");
        // monotonic rule still holds: out-of-order completion of newer first, older is stale (not superseded) and unknown sid 0 / full table is never skipped
        n48df_t m2 = active(); int h0 = n48df_pick(&m2, all), h1 = n48df_pick(&m2, all); n48df_submit(&m2, h0, 4, 40); n48df_submit(&m2, h1, 4, 41);
        CHECK("monotonic: newest first presents", n48df_complete_surf(&m2, h1, 0, 41, 4) == 1, "seq 41");
        CHECK("monotonic: the older then is stale, not superseded", n48df_complete_surf(&m2, h0, 0, 40, 4) == 0 && m2.stale == 1 && m2.superseded == 0, "stale 1");
        n48df_t u0 = active(); int k0 = n48df_pick(&u0, all); n48df_submit(&u0, k0, 0, 5);
        CHECK("unknown surface id is never tracked or skipped", n48df_complete_surf(&u0, k0, 0, 1, 0) == 1 && u0.superseded == 0, "sid 0");
        n48df_t ft = active(); int k1 = n48df_pick(&ft, all); n48df_submit(&ft, -1, 101, 1); n48df_submit(&ft, 2, 102, 2); n48df_submit(&ft, k1, 999, 50);   // a submission with no slot in flight is untracked (bundle 20: no surface table to fill)
        CHECK("untracked submissions are counted and never skip", ft.surf_untracked == 2 && !n48df_is_superseded(&ft, k1) && n48df_complete_surf(&ft, k1, 0, 49, 999) == 1 && ft.superseded == 0, "no skip, surf_untracked %llu", (unsigned long long)ft.surf_untracked);
        n48df_t q3 = active(); int k2 = n48df_pick(&q3, all); n48df_submit(&q3, k2, 6, 9); n48df_fail(&q3, N48DF_R_PLANELOST);
        CHECK("OFF: no present, no superseded count", n48df_complete_surf(&q3, k2, 0, 8, 6) == 0 && q3.superseded == 0, "closed");
        // 100k random soak: submissions in seq order over 3 surfaces (random drop = no submit), completions in any order, random GPU failures
        srand(4242); n48df_t r3 = active(); int held3[3] = { 0, 0, 0 }; uint64_t sq3[3] = { 0, 0, 0 }, next3 = 0, lastp3 = 0, viol3 = 0, np3 = 0, nsk = 0; uint32_t sid3[3] = { 0, 0, 0 };
        for (int i = 0; i < 100000; i++) {
            int pk = n48df_pick(&r3, all);
            if (pk >= 0) { held3[pk] = 1; sq3[pk] = ++next3; sid3[pk] = 1 + (uint32_t)(rand() % 3); n48df_submit(&r3, pk, sid3[pk], sq3[pk]); }
            int cs = rand() % 3;
            if (held3[cs] && (rand() & 1)) {
                int fail = (rand() % 13) == 0; uint64_t sup0 = r3.superseded;
                int w = n48df_complete_surf(&r3, cs, fail ? -3 : 0, sq3[cs], sid3[cs]); held3[cs] = 0; nsk += r3.superseded - sup0;
                if (w) { np3++; n48df_present_result(&r3, 0); if (sq3[cs] <= lastp3 || fail) viol3++; lastp3 = sq3[cs]; }
                else if (!fail && r3.superseded == sup0 && sq3[cs] > lastp3) viol3++;   // a successful frame newer than the last presented and not superseded must present
            }
            for (int k = 0; k < 3; k++) if (held3[k] != r3.inflight[k]) viol3++;
        }
        CHECK("soak 100000 superseded steps", viol3 == 0 && np3 == r3.presents && nsk == r3.superseded && nsk > 0 && r3.state == N48DF_ACTIVE, "%llu violations, %llu presented, %llu superseded, %llu stale, %llu gpu failures", (unsigned long long)viol3, (unsigned long long)np3, (unsigned long long)r3.superseded, (unsigned long long)r3.stale, (unsigned long long)r3.gpu_fail);
    }

    // ---- native #12 P2: present only CoreDisplay's final (GPUPass) frames ----
    {
        enum { P = N48DF_W_PASS, CL = N48DF_W_CLEAR, DF = N48DF_W_DRAW_FINAL, DO = N48DF_W_DRAW_OTHER, BL = N48DF_W_BLIT, CP = N48DF_W_COMPUTE };
        CHECK("fn: GPUPass and specialisations are final", n48df_fn_is_final("GPUPass") && n48df_fn_is_final("GPUPass_spec3") && !n48df_fn_is_final("ViewportToNDC") && !n48df_fn_is_final("") && !n48df_fn_is_final(0) && !n48df_fn_is_final("gpupass"), "fragment name match is case-sensitive substring");
        struct { const char *n; uint32_t m; int cls, pres; } t[] = {
            { "final: pass + GPUPass draw",              P | DF,           N48DF_C_FINAL,        1 },
            { "final: clear load + GPUPass draw",        P | CL | DF,      N48DF_C_FINAL,        1 },
            { "mixed: GPUPass draw + other draw",        P | DF | DO,      N48DF_C_FINAL,        1 },
            { "mixed: GPUPass draw + blit",              P | DF | BL,      N48DF_C_FINAL,        1 },
            { "mixed: GPUPass draw + compute",           P | DF | CP,      N48DF_C_FINAL,        1 },
            { "non-final: other-function draw",          P | DO,           N48DF_C_RENDER_OTHER, 0 },
            { "non-final: clear + other draw",           P | CL | DO,      N48DF_C_RENDER_OTHER, 0 },
            { "non-final: clear-only pass (ColorFill)",  P | CL,           N48DF_C_CLEAR_ONLY,   0 },
            { "non-final: load pass with no draw",       P,                N48DF_C_PASS_NODRAW,  0 },
            { "non-final: blit",                         BL,               N48DF_C_BLIT,         0 },
            { "non-final: blit + clear-only pass",       P | CL | BL,      N48DF_C_BLIT,         0 },
            { "non-final: compute",                      CP,               N48DF_C_COMPUTE,      0 },
            { "non-final: compute + other draw",         P | DO | CP,      N48DF_C_RENDER_OTHER, 0 },
            { "non-final: nothing recorded",             0,                N48DF_C_UNKNOWN,      0 },
        };
        for (unsigned i = 0; i < sizeof t / sizeof t[0]; i++) {
            n48df_t d = active(); int c = n48df_class(t[i].m), r = n48df_account(&d, t[i].m);
            CHECK(t[i].n, c == t[i].cls && r == t[i].pres && d.cls[c] == 1 && d.nonfinal[c] == (uint64_t)!t[i].pres, "class %s, %s, nonfinal_skipped %llu", n48df_cls_name[c], r ? "copy+present" : "no copy, no present", (unsigned long long)d.nonfinal[c]);
            n48df_t pa = active(); pa.presentall = 1;
            CHECK("  presentall kill switch presents it", n48df_account(&pa, t[i].m) == 1 && pa.nonfinal[c] == 0 && pa.cls[c] == 1, "class counted, never skipped");
        }
        // composition with the other rules: a skipped (non-final) cb takes no slot, is never submit()ed, so it does not supersede an earlier final frame of the same surface
        n48df_t d = active(); int s0 = n48df_pick(&d, all); n48df_submit(&d, s0, 55, 1);          // final frame, seq 1, surface 55
        int nf = n48df_account(&d, P | CL);                                                   // a later clear-only write of surface 55: refused, so no pick / submit / seq
        CHECK("non-final does not supersede the earlier final frame", nf == 0 && n48df_complete_surf(&d, s0, 0, 1, 55) == 1 && d.superseded == 0 && d.picks == 1, "final frame presents, no slot taken by the non-final");
        // a later FINAL frame still supersedes an earlier final frame (existing rule kept) and monotonic order still holds
        n48df_t m = active(); int a0 = n48df_pick(&m, all), a1 = n48df_pick(&m, all); n48df_submit(&m, a0, 9, 1); n48df_submit(&m, a1, 9, 2);
        CHECK("final frames keep the superseded rule", n48df_account(&m, P | DF) && n48df_complete_surf(&m, a0, 0, 1, 9) == 0 && m.superseded == 1 && n48df_complete_surf(&m, a1, 0, 2, 9) == 1, "seq 1 skipped, seq 2 presents");
        // a 10000-cb mixed stream: slots taken == final count
        srand(777); n48df_t st = active(); uint64_t nfin = 0;
        for (int i = 0; i < 10000; i++) { uint32_t mm = (uint32_t)(rand() & 63); nfin += (mm & DF) != 0; n48df_account(&st, mm); }
        uint64_t sk = 0; for (int i = 1; i < N48DF_NCLS; i++) sk += st.nonfinal[i];
        CHECK("stream: finals counted, rest skipped", st.cls[N48DF_C_FINAL] == nfin && st.nonfinal[N48DF_C_FINAL] == 0 && sk == 10000 - nfin, "%llu final, %llu skipped", (unsigned long long)nfin, (unsigned long long)sk);
    }

    // ================= native #12 D1/D2: rectangles, region builder, partial-update chain =================
    {
        typedef n48df_rect_t RC;
        RC a = { 10, 10, 20, 20 }, b = { 15, 5, 30, 12 }, e = { 5, 5, 5, 9 }, z0 = { 0, 0, 0, 0 };
        RC u = n48df_rect_union(a, b), i = n48df_rect_isect(a, b);
        CHECK("rect union", u.x0 == 10 && u.y0 == 5 && u.x1 == 30 && u.y1 == 20, "[%d,%d)-[%d,%d)", u.x0, u.y0, u.x1, u.y1);
        CHECK("rect isect", i.x0 == 15 && i.y0 == 10 && i.x1 == 20 && i.y1 == 12, "[%d,%d)-[%d,%d)", i.x0, i.y0, i.x1, i.y1);
        CHECK("rect union ignores empty", memcmp(&(RC){ 0 }, &z0, sizeof z0) == 0 && n48df_rect_union(a, e).x0 == 10 && n48df_rect_union(e, a).y1 == 20 && n48df_rect_empty(n48df_rect_union(e, z0)), "empty operand ignored");
        CHECK("rect isect disjoint is empty", n48df_rect_empty(n48df_rect_isect(a, (RC){ 20, 0, 30, 30 })) && n48df_rect_empty(n48df_rect_isect(a, (RC){ 0, 20, 30, 30 })), "touching edges do not intersect");
        RC c = n48df_rect_clip((RC){ -50, -3, 5000, 1500 }, 2560, 1440);
        CHECK("rect clip", c.x0 == 0 && c.y0 == 0 && c.x1 == 2560 && c.y1 == 1440 && n48df_rect_is_full(c, 2560, 1440), "clipped to the surface -> full");
        CHECK("rect not full", !n48df_rect_is_full((RC){ 0, 0, 2560, 1439 }, 2560, 1440) && !n48df_rect_is_full((RC){ 1, 0, 2560, 1440 }, 2560, 1440) && !n48df_rect_is_full(z0, 0, 0), "one row / column short is partial; 0x0 surface never full");
        RC d1 = n48df_draw_rect(0, 0, 2560, 1440, 0, 0, 2560, 1440, 2560, 1440);
        CHECK("draw rect: full viewport + full scissor", n48df_rect_is_full(d1, 2560, 1440), "full");
        RC d2 = n48df_draw_rect(0, 0, 2560, 1440, 100, 200, 300, 40, 2560, 1440);
        CHECK("draw rect: scissor narrows a full viewport", d2.x0 == 100 && d2.y0 == 200 && d2.x1 == 400 && d2.y1 == 240, "[%d,%d)-[%d,%d)", d2.x0, d2.y0, d2.x1, d2.y1);
        RC d3 = n48df_draw_rect(100.5, 50.25, 200, 100, 0, 0, 2560, 1440, 2560, 1440);
        CHECK("draw rect: fractional viewport rounds outward", d3.x0 <= 100 && d3.y0 <= 50 && d3.x1 >= 301 && d3.y1 >= 151 && d3.x1 <= 303 && d3.y1 <= 153, "[%d,%d)-[%d,%d)", d3.x0, d3.y0, d3.x1, d3.y1);
        RC d4 = n48df_draw_rect(-100, -100, 300, 300, 0, 0, 5000, 5000, 2560, 1440);
        CHECK("draw rect: viewport partly off-screen is clipped", d4.x0 == 0 && d4.y0 == 0 && d4.x1 >= 200 && d4.x1 <= 202 && d4.y1 >= 200 && d4.y1 <= 202, "[%d,%d)-[%d,%d)", d4.x0, d4.y0, d4.x1, d4.y1);
        CHECK("draw rect: degenerate / absurd inputs are empty or clipped", n48df_rect_empty(n48df_draw_rect(0, 0, 0, 10, 0, 0, 100, 100, 100, 100)) && n48df_rect_empty(n48df_draw_rect(0, 0, 10, 10, 0, 0, 0, 5, 100, 100)) &&
              n48df_rect_empty(n48df_draw_rect(0, 0, 10, -5, 0, 0, 100, 100, 100, 100)) && n48df_rect_empty(n48df_draw_rect(1e300, 0, 10, 10, 0, 0, 100, 100, 100, 100)) &&
              n48df_rect_empty(n48df_draw_rect(0, 0, 10, 10, 200, 200, 5, 5, 100, 100)) && n48df_rect_is_full(n48df_draw_rect(0, 0, 100, 100, -(1LL << 40), -(1LL << 40), 1LL << 41, 1LL << 41, 100, 100), 100, 100), "empty or clipped, no overflow");
        // accumulator + resolve
        n48df_dmg_t dm; memset(&dm, 0, sizeof dm); RC r;
        CHECK("resolve: no draw -> NONE", n48df_dmg_resolve(&dm, 2560, 1440, &r) == N48DF_K_NONE, "none");
        n48df_dmg_add_draw(&dm, (RC){ 100, 100, 200, 200 }, 2560, 1440); n48df_dmg_add_draw(&dm, (RC){ 150, 50, 300, 120 }, 2560, 1440);
        CHECK("resolve: union of two partial draws", n48df_dmg_resolve(&dm, 2560, 1440, &r) == N48DF_K_PART && r.x0 == 100 && r.y0 == 50 && r.x1 == 300 && r.y1 == 200 && dm.draws == 2 && dm.part_draws == 2 && dm.full_draws == 0, "[%d,%d)-[%d,%d)", r.x0, r.y0, r.x1, r.y1);
        n48df_dmg_add_draw(&dm, (RC){ 0, 0, 2560, 1440 }, 2560, 1440);
        CHECK("resolve: a full-screen draw makes it FULL", n48df_dmg_resolve(&dm, 2560, 1440, &r) == N48DF_K_FULL && dm.full_draws == 1, "full");
        n48df_dmg_t d5; memset(&d5, 0, sizeof d5); n48df_dmg_add_draw(&d5, (RC){ 0, 0, 0, 0 }, 2560, 1440);
        CHECK("resolve: draws that clip away are PART with an empty rectangle", n48df_dmg_resolve(&d5, 2560, 1440, &r) == N48DF_K_PART && n48df_rect_empty(r), "empty");
        n48df_dmg_t d6; memset(&d6, 0, sizeof d6); n48df_dmg_add_draw(&d6, (RC){ 1, 1, 2, 2 }, 2560, 1440); n48df_dmg_set_full(&d6);
        CHECK("resolve: clear/blit/compute (full flag) beats partial draws; unknown size is FULL", n48df_dmg_resolve(&d6, 2560, 1440, &r) == N48DF_K_FULL && n48df_dmg_resolve(&d5, 0, 0, &r) == N48DF_K_FULL, "full");

        // ---- region builder ----
        n48df_region_t rg[1500]; const uint32_t P = 10240;
        uint32_t n = n48df_regions((RC){ 0, 100, 2560, 300 }, 2560, 1440, P, 4, rg, 1500);
        CHECK("regions: full width = ONE contiguous region", n == 1 && rg[0].src == 100ull * P && rg[0].dst == rg[0].src && rg[0].size == 200ull * P, "off %llu size %llu", (unsigned long long)rg[0].src, (unsigned long long)rg[0].size);
        n = n48df_regions((RC){ 10, 5, 30, 8 }, 2560, 1440, P, 4, rg, 1500);
        CHECK("regions: partial width = one per row", n == 3 && rg[0].src == 5ull * P + 40 && rg[0].size == 80 && rg[2].src == 7ull * P + 40 && rg[2].dst == rg[2].src, "%u rows, row 0 at %llu", n, (unsigned long long)rg[0].src);
        n = n48df_regions((RC){ -20, -20, 8, 3 }, 2560, 1440, P, 4, rg, 1500);
        CHECK("regions: clipped to the surface", n == 3 && rg[0].src == 0 && rg[0].size == 32 && rg[2].src == 2ull * P, "%u rows", n);
        CHECK("regions: empty / off-surface = no copy", n48df_regions((RC){ 5, 5, 5, 9 }, 2560, 1440, P, 4, rg, 1500) == 0 && n48df_regions((RC){ 3000, 0, 3100, 10 }, 2560, 1440, P, 4, rg, 1500) == 0 && n48df_regions((RC){ 0, 1440, 100, 1500 }, 2560, 1440, P, 4, rg, 1500) == 0, "0 regions");
        n = n48df_regions((RC){ 0, 1439, 2560, 2000 }, 2560, 1440, P, 4, rg, 1500);
        CHECK("regions: last row stays inside the surface", n == 1 && rg[0].src + rg[0].size == 1440ull * P, "ends at %llu", (unsigned long long)(rg[0].src + rg[0].size));
        n = n48df_regions((RC){ 0, 0, 2559, 1440 }, 2560, 1440, P, 4, rg, 1500);
        CHECK("regions: one column short = 1440 rows", n == 1440 && rg[1439].src == 1439ull * P && rg[1439].size == 2559ull * 4, "%u", n);
        CHECK("regions: too many rows for the array", n48df_regions((RC){ 0, 0, 100, 1440 }, 2560, 1440, P, 4, rg, 100) == UINT32_MAX, "UINT32_MAX (caller copies the whole frame)");
        n = n48df_regions((RC){ 0, 10, 16, 12 }, 16, 100, 80, 4, rg, 1500);
        CHECK("regions: pitch wider than the row = per-row even at full width", n == 2 && rg[0].src == 800 && rg[0].size == 64 && rg[1].src == 880, "%u rows, size %llu", n, (unsigned long long)rg[0].size);
        // byte-exact model: base slot A (0xAA) -> new slot; region from S (0x55) over it; only rows of R inside R changed
        {
            enum { W = 20, H = 12, PP = 96 };     // pitch 96 > 80
            static uint8_t base[PP * H], surf[PP * H], neu[PP * H];
            memset(base, 0xAA, sizeof base); memset(surf, 0x55, sizeof surf); memcpy(neu, base, sizeof base);
            RC q = { 3, 2, 17, 9 }; n48df_region_t rr[64]; uint32_t nn = n48df_regions(q, W, H, PP, 4, rr, 64);
            for (uint32_t k = 0; k < nn; k++) memcpy(neu + rr[k].dst, surf + rr[k].src, rr[k].size);
            int bad = 0; for (int y = 0; y < H; y++) for (int x = 0; x < PP; x++) { int inR = x < W * 4 && x >= q.x0 * 4 && x < q.x1 * 4 && y >= q.y0 && y < q.y1; if (neu[y * PP + x] != (inR ? 0x55 : 0xAA)) bad++; }
            CHECK("regions: byte-exact model (base outside R, surface inside R, pitch padding untouched)", nn == 7 && bad == 0, "%u regions, %d wrong bytes", nn, bad);
        }
        // ---- classification with the new bits ----
        enum { P_ = N48DF_W_PASS, DF_ = N48DF_W_DRAW_FINAL, DO_ = N48DF_W_DRAW_OTHER, FL_ = N48DF_W_DRAW_FILL, CO_ = N48DF_W_DRAW_COMP, BL_ = N48DF_W_BLIT };
        CHECK("draw bits by fragment name", n48df_draw_bits("GPUPass") == DF_ && n48df_draw_bits("ColorFill") == (DO_ | FL_) && n48df_draw_bits("ColorFillYCbCr") == (DO_ | FL_) && n48df_draw_bits("UberCompositeFragment") == (DO_ | CO_) && n48df_draw_bits("VfxFoo") == (DO_ | CO_) && n48df_draw_bits(NULL) == (DO_ | CO_), "GPUPass / ColorFill* / anything else");
        CHECK("class: composite", n48df_class(P_ | DO_ | CO_) == N48DF_C_RENDER_COMP && n48df_class(P_ | DO_ | FL_) == N48DF_C_RENDER_OTHER && n48df_class(P_ | DF_ | CO_) == N48DF_C_FINAL && !strcmp(n48df_cls_name[N48DF_C_RENDER_COMP], "render-composite"), "composite-only is its own class; ColorFill stays render-other; GPUPass wins");
        CHECK("should_present: damage off", !n48df_should_present(P_ | DO_ | CO_, 0, 0) && n48df_should_present(P_ | DF_, 0, 0) && !n48df_should_present(P_ | DO_ | FL_, 0, 0), "composite is NOT a frame while the surface is not a scanout surface");
        CHECK("should_present: scanout flag set", n48df_should_present(P_ | DO_ | CO_, 0, 1) && n48df_should_present(P_ | DF_, 0, 1) && !n48df_should_present(P_ | DO_ | FL_, 0, 1) && !n48df_should_present(P_ | BL_, 0, 1) && n48df_should_present(P_ | DO_ | FL_ | CO_, 0, 1), "composite is a frame; ColorFill-only stays non-final; fill + composite is a frame");
        for (int dmg = 0; dmg < 2; dmg++) {   // native #12 scanout flag: the m13-s0 sid pattern, damage off and on
            n48df_t d = active(); d.damage = dmg; int pres[4] = { 0, 0, 0, 0 };
            const uint32_t sids[4] = { 1, 198, 2, 3 };
            CHECK("scanout: unflagged composite refused", n48df_account_sid(&d, P_ | DO_ | CO_, 1, 0) == 0 && !n48df_is_scan(&d, 1) && d.nonscan_total == 1, "sid 1 composite-only never presented (damage %d)", dmg);
            for (int r = 0; r < 50; r++) {
                pres[0] += n48df_account_sid(&d, P_ | DO_ | CO_, 1, 0);          // SkyLight composite sources, never GPUPass
                pres[1] += n48df_account_sid(&d, P_ | DO_ | CO_, 198, 0);
                int ds = (r & 1) ? 3 : 2;                                      // CoreDisplay's display surfaces alternate, GPUPass + composite
                pres[2] += n48df_account_sid(&d, P_ | DF_, ds, 0); pres[3] += n48df_account_sid(&d, P_ | DO_ | CO_, ds, 0);
            }
            CHECK("scanout: m13-s0 pattern", pres[0] == 0 && pres[1] == 0 && pres[2] == 50 && pres[3] == 50 && !n48df_is_scan(&d, sids[0]) && !n48df_is_scan(&d, sids[1]) && n48df_is_scan(&d, 2) && n48df_is_scan(&d, 3),
                  "1/198 composite-only: 0 presented; 2/3 GPUPass+composite: 100 presented (damage %d)", dmg);
            uint64_t s1 = 0, s198 = 0; for (int i = 0; i < N48DF_MAX_SURF; i++) { if (d.nonscan_skip[i] && d.nonscan_sid[i] == 1) s1 = d.nonscan_skip[i]; if (d.nonscan_skip[i] && d.nonscan_sid[i] == 198) s198 = d.nonscan_skip[i]; }
            CHECK("scanout: skips counted by sid; flag log", s1 == 51 && s198 == 50 && d.nonscan_total == 101 && d.nflag_log == 2 && d.flag_log[0] == 2 && d.flag_log[1] == 3 && d.nscan == 2, "sid1 %llu sid198 %llu total %llu, flag events %d", (unsigned long long)s1, (unsigned long long)s198, (unsigned long long)d.nonscan_total, d.nflag_log);
            CHECK("scanout: fill-only never presents, even when flagged", n48df_account_sid(&d, P_ | DO_ | FL_, 2, 0) == 0 && n48df_account_sid(&d, P_ | BL_, 3, 0) == 0 && d.nonscan_total == 101, "ColorFill/blit stay non-final");
            n48df_t pa = active(); pa.presentall = 1;
            CHECK("scanout: presentall restores the old rule", n48df_account_sid(&pa, P_ | DO_ | CO_, 198, 0) == 1 && n48df_account_sid(&pa, P_ | DO_ | FL_, 1, 0) == 1 && pa.nonscan_total == 0, "everything presents");
            n48df_t u = active(); CHECK("scanout: sid 0 (unknown) final presents, composite does not", n48df_account_sid(&u, P_ | DF_, 0, 0) == 1 && !n48df_is_scan(&u, 0) && n48df_account_sid(&u, P_ | DO_ | CO_, 0, 0) == 0, "no flag for sid 0");
            n48df_t f = active(); int fl = 0; for (uint32_t k = 1; k <= 12; k++) fl += n48df_account_sid(&f, P_ | DF_, 100 + k, 0) == 1;
            CHECK("scanout: flag table full -> composite refused, final still presents", fl == 12 && f.nscan == N48DF_MAX_SURF && f.nflag_log == 8 && n48df_account_sid(&f, P_ | DO_ | CO_, 111, 0) == 0, "8 flagged, flag log %d", f.nflag_log);
        }

        // ---- D1 accounting ----
        { n48df_t d = active(); n48df_wr_t w; memset(&w, 0, sizeof w); n48df_dmg_add_draw(&w.all, (RC){ 0, 0, 100, 100 }, 2560, 1440); w.src_kind = 2;
          n48df_dmg_account(&d, N48DF_C_FINAL, &w, 2560, 1440);
          n48df_wr_t w2; memset(&w2, 0, sizeof w2); n48df_dmg_add_draw(&w2.all, (RC){ 0, 0, 2560, 1440 }, 2560, 1440); n48df_dmg_account(&d, N48DF_C_RENDER_OTHER, &w2, 2560, 1440);
          n48df_wr_t w3; memset(&w3, 0, sizeof w3); n48df_dmg_set_full(&w3.all); n48df_dmg_account(&d, N48DF_C_CLEAR_ONLY, &w3, 2560, 1440);
          CHECK("D1 accounting by class", d.dk[N48DF_C_FINAL][N48DF_K_PART] == 1 && d.dk_area[N48DF_C_FINAL] == 10000 && d.srck[2] == 1 && d.dk[N48DF_C_RENDER_OTHER][N48DF_K_FULL] == 1 && d.dd_full[N48DF_C_RENDER_OTHER] == 1 && d.dd_part[N48DF_C_FINAL] == 1 && d.dk[N48DF_C_CLEAR_ONLY][N48DF_K_FULL] == 1, "partial 10000 px, source kind counted for GPUPass only"); }

        // ---- plan / chain ----
        {
            n48df_t d = active(); d.damage = 1; n48df_plan_t p; RC R1 = { 100, 100, 200, 200 };
            int s1 = n48df_plan(&d, all, N48DF_K_PART, R1, &p);
            CHECK("plan: first frame has no front -> full copy (restart)", s1 >= 0 && p.base == -1 && p.kind == N48DF_K_FULL && p.id == 1 && p.baseid == 0 && d.dm_restart == 1 && d.head == s1, "slot %d full", s1);
            int s2 = n48df_plan(&d, all, N48DF_K_PART, R1, &p);
            CHECK("plan: second frame = base is the first, rect kept", s2 >= 0 && s2 != s1 && p.base == s1 && p.baseid == 1 && p.id == 2 && p.kind == N48DF_K_PART && p.rect.x0 == 100 && d.pin[s1] == 1 && d.basep[s2] == s1 && d.dm_part == 1, "slot %d base %d", s2, p.base);
            int s3 = n48df_plan(&d, all, N48DF_K_PART, R1, &p);
            CHECK("plan: third frame cannot take the pinned base nor the head", s3 >= 0 && s3 != s1 && s3 != s2 && p.base == s2 && d.pin[s2] == 1, "slot %d base %d", s3, p.base);
            CHECK("plan: all slots busy -> drop, damage carried", n48df_plan(&d, all, N48DF_K_PART, (RC){ 500, 500, 600, 600 }, &p) == -1 && d.drops == 1 && d.dm_carry == 1 && !d.carry_full && d.carry[0] == 500, "carry [%d,%d)", d.carry[0], d.carry[2]);
            CHECK("complete unpins the base", n48df_complete_seq(&d, s2, 0, 1) == 1 && d.pin[s1] == 0 && d.basep[s2] == -1 && d.pin[s2] == 1, "pin[%d] = %u (still the base of the third frame)", s1, d.pin[s1]);
            n48df_complete_seq(&d, s1, 0, 2);
            int s4 = n48df_plan(&d, all, N48DF_K_PART, (RC){ 700, 10, 710, 20 }, &p);
            CHECK("plan: the dropped frame's damage is merged into the next rectangle", s4 >= 0 && p.kind == N48DF_K_PART && p.rect.x0 == 500 && p.rect.y0 == 10 && p.rect.x1 == 710 && p.rect.y1 == 600 && d.carry[0] == 0 && d.carry[2] == 0, "[%d,%d)-[%d,%d) base %d", p.rect.x0, p.rect.y0, p.rect.x1, p.rect.y1, p.base);
            n48df_complete_seq(&d, s3, 0, 3); n48df_complete_seq(&d, s4, 0, 4);
            CHECK("everything released: no pin, no inflight", n48df_inflight_count(&d) == 0 && !d.pin[0] && !d.pin[1] && !d.pin[2], "clean");
            // a FULL frame restarts the chain; an unknown (carry_full) too
            int s5 = n48df_plan(&d, all, N48DF_K_FULL, (RC){ 0, 0, 2560, 1440 }, &p);
            CHECK("plan: FULL kind = full copy, chain restarts", s5 >= 0 && p.base == -1 && d.head == s5 && d.dm_full == 1, "full");
            n48df_complete_seq(&d, s5, 0, 5);
            n48df_t d2 = active(); d2.damage = 1; int a0 = n48df_plan(&d2, all, N48DF_K_PART, R1, &p); int a1 = n48df_plan(&d2, all, N48DF_K_PART, R1, &p); int a2 = n48df_plan(&d2, all, N48DF_K_FULL, R1, &p); (void)a0; (void)a1;
            CHECK("full frame needs no base: no pin taken", a2 >= 0 && p.base == -1 && d2.pin[0] + d2.pin[1] + d2.pin[2] == 1, "only the second frame's base is pinned");
            n48df_t d3 = active(); d3.damage = 1; n48df_plan(&d3, all, N48DF_K_PART, R1, &p); n48df_complete_seq(&d3, d3.head, 0, 1);
            n48df_plan(&d3, none, N48DF_K_FULL, R1, &p);
            int sx = n48df_plan(&d3, all, N48DF_K_PART, R1, &p);
            CHECK("a dropped FULL/unknown frame forces the next one to copy everything", sx >= 0 && p.base == -1 && p.kind == N48DF_K_FULL && !d3.carry_full, "carry_full consumed");
            // damage off: no chain, no avoid, behaviour == the old pick
            n48df_t of = active(); int o1 = n48df_plan(&of, all, N48DF_K_PART, R1, &p); int o2 = n48df_plan(&of, all, N48DF_K_PART, R1, &p);
            CHECK("damage off: full copies, no chain ids, no pins", o1 == 0 && o2 == 1 && p.base == -1 && p.id == 0 && of.head == -1 && of.pin[0] == 0 && of.dm_part == 0 && of.dm_full == 0, "slots %d %d", o1, o2);
            // GPU failure resets the chain
            n48df_t gf = active(); gf.damage = 1; n48df_plan(&gf, all, N48DF_K_PART, R1, &p); int g1 = n48df_plan(&gf, all, N48DF_K_PART, R1, &p);
            n48df_complete_seq(&gf, g1, -4, 1);
            int g2 = n48df_plan(&gf, all, N48DF_K_PART, R1, &p);
            CHECK("GPU failure: head forgotten, next frame is a full copy, pins released", g2 >= 0 && p.base == -1 && gf.pin[0] + gf.pin[1] + gf.pin[2] == 0, "restart after fail");
            // chain submit validity
            n48df_t cs = active(); cs.damage = 1; n48df_plan_t pa, pb;
            int c1 = n48df_plan(&cs, all, N48DF_K_PART, R1, &pa), c2 = n48df_plan(&cs, all, N48DF_K_PART, R1, &pb);
            n48df_chain_submit(&cs, c1, pa.id, pa.baseid); n48df_chain_submit(&cs, c2, pb.id, pb.baseid);
            CHECK("in-order submit is valid", !cs.poison[c1] && !cs.poison[c2] && cs.head >= 0 && cs.last_sub_valid, "no poison");
            n48df_t cr = active(); cr.damage = 1;
            c1 = n48df_plan(&cr, all, N48DF_K_PART, R1, &pa); c2 = n48df_plan(&cr, all, N48DF_K_PART, R1, &pb);
            n48df_chain_submit(&cr, c2, pb.id, pb.baseid);        // the dependent frame reaches the queue FIRST
            CHECK("out-of-order submit poisons the dependent and restarts the chain", cr.poison[c2] && cr.head == -1 && !cr.last_sub_valid, "poisoned");
            n48df_chain_submit(&cr, c1, pa.id, pa.baseid);
            CHECK("the full base frame itself is still valid", !cr.poison[c1] && cr.last_sub_valid, "valid");
            CHECK("poisoned frame is never presented and counted", n48df_complete_seq(&cr, c2, 0, 1) == 0 && cr.chain_invalid == 1 && cr.pin[c1] == 0 && n48df_complete_seq(&cr, c1, 0, 2) == 1, "chain_invalid %llu, the base frame presents", (unsigned long long)cr.chain_invalid);
            n48df_t ch = active(); ch.damage = 1;
            c1 = n48df_plan(&ch, all, N48DF_K_PART, R1, &pa); c2 = n48df_plan(&ch, all, N48DF_K_PART, R1, &pb); int c3 = -1; n48df_plan_t pc;
            n48df_complete_seq(&ch, c1, 0, 1);
            c3 = n48df_plan(&ch, all, N48DF_K_PART, R1, &pc);
            n48df_chain_submit(&ch, c1, pa.id, pa.baseid); n48df_chain_submit(&ch, c3, pc.id, pc.baseid);   // the middle frame c2 was never submitted
            CHECK("a base that was never submitted poisons its dependent", ch.poison[c3] && !ch.poison[c1], "poison on the frame whose base never reached the queue");
            (void)c2;
        }
        // ---- chain soak with a content-version model: a non-poisoned frame must read its base slot exactly as the base frame left it ----
        {
            srand(4242); n48df_t sm = active(); sm.damage = 1;
            enum { MAXF = 64 };
            struct fr { int live, slot, sub, done; n48df_plan_t pl; uint64_t seq; } fq[MAXF]; memset(fq, 0, sizeof fq);
            uint64_t slotver[3] = { 0, 0, 0 }, nextseq = 0, bad = 0, pres = 0, subs = 0, enc = 0, poisoned = 0; uint64_t lastpres_ver[3] = { 0, 0, 0 };
            for (int step = 0; step < 300000; step++) {
                int op = rand() % 3, nlive = 0; for (int q = 0; q < MAXF; q++) nlive += fq[q].live;
                if (nlive >= 3 && op == 0) op = 1 + rand() % 2;
                if (op == 0) {            // encode
                    int f = -1; for (int k = 0; k < MAXF; k++) if (!fq[k].live) { f = k; break; }
                    if (f >= 0) {
                        uint32_t fl[3]; for (int k = 0; k < 3; k++) fl[k] = (rand() % 5) ? R : 0;
                        int kind = (rand() % 10) < 7 ? N48DF_K_PART : N48DF_K_FULL;
                        RC rc = { rand() % 50, rand() % 50, 50 + rand() % 50, 50 + rand() % 50 };
                        int sl = n48df_plan(&sm, fl, kind, rc, &fq[f].pl);
                        if (sl >= 0) { fq[f].live = 1; fq[f].slot = sl; fq[f].sub = 0; fq[f].done = 0; enc++; }
                    }
                } else if (op == 1) {     // submit a random encoded frame (any order), or cancel it
                    int f = -1, cnt = 0; for (int q = 0; q < MAXF; q++) if (fq[q].live && !fq[q].sub && (rand() % ++cnt) == 0) f = q;
                    if (f >= 0) {
                        if ((rand() % 40) == 0) { n48df_complete_seq(&sm, fq[f].slot, -1, 0); fq[f].live = 0; }   // cancelled: never submitted
                        else {
                            fq[f].sub = 1; fq[f].seq = ++nextseq; subs++;
                            n48df_chain_submit(&sm, fq[f].slot, fq[f].pl.id, fq[f].pl.baseid);
                            if (sm.poison[fq[f].slot]) poisoned++;
                            else {   // the GPU runs the copies now (in submit order)
                                if (fq[f].pl.base >= 0 && slotver[fq[f].pl.base] != fq[f].pl.baseid) { bad++; if (getenv("DBG")) printf("BASE MISMATCH base slot %d has ver %llu want %llu (frame id %llu slot %d)\n", fq[f].pl.base, (unsigned long long)slotver[fq[f].pl.base], (unsigned long long)fq[f].pl.baseid, (unsigned long long)fq[f].pl.id, fq[f].slot); }
                                slotver[fq[f].slot] = fq[f].pl.id ? fq[f].pl.id : 1000000 + nextseq;
                            }
                            if (sm.poison[fq[f].slot]) slotver[fq[f].slot] = 0;
                        }
                    }
                } else {                  // complete a random submitted frame
                    int f = -1, cnt = 0; for (int q = 0; q < MAXF; q++) if (fq[q].live && fq[q].sub && !fq[q].done && (rand() % ++cnt) == 0) f = q;
                    if (f >= 0) {
                        int fail = (rand() % 25) == 0; int wp = sm.poison[fq[f].slot];
                        int pr = n48df_complete_seq(&sm, fq[f].slot, fail ? -4 : 0, fq[f].seq); fq[f].done = 1; fq[f].live = 0;
                        if (fail) slotver[fq[f].slot] = 0;
                        if (pr) { pres++; if (wp || slotver[fq[f].slot] != fq[f].pl.id) { bad++; if (getenv("DBG")) printf("PRESENT MISMATCH slot %d ver %llu id %llu wp %d\n", fq[f].slot, (unsigned long long)slotver[fq[f].slot], (unsigned long long)fq[f].pl.id, wp); } lastpres_ver[fq[f].slot] = fq[f].pl.id; n48df_present_result(&sm, 0); }
                    }
                }
                for (int k = 0; k < 3; k++) { int expect = 0; for (int q = 0; q < MAXF; q++) if (fq[q].live && fq[q].pl.base == k) expect++; if (sm.pin[k] != expect) { bad++; if (getenv("DBG")) printf("PIN MISMATCH slot %d pin %d expect %d step %d\n", k, sm.pin[k], expect, step); } }
            }
            CHECK("chain soak 300000 steps: no frame ever read a base the GPU had not written; pins exact", bad == 0 && pres > 20000 && enc > 50000 && sm.dm_part > 10000, "%llu violations, %llu encoded, %llu submitted, %llu presented, %llu poisoned, partial %llu full %llu restarts %llu carried %llu", (unsigned long long)bad, (unsigned long long)enc, (unsigned long long)subs, (unsigned long long)pres, (unsigned long long)poisoned, (unsigned long long)sm.dm_part, (unsigned long long)sm.dm_full, (unsigned long long)sm.dm_restart, (unsigned long long)sm.dm_carry);
            (void)lastpres_ver;
        }
    }


    // ---- native #12 P3 present hold ----
    {
        hsim_t h; n48df_t hs; int r;
        r = hold_frame(1, 3, 1, 2580000ull, &h, &hs);
        CHECK("hold: G then C within H -> G dropped, C presented", r == 2 && hs.superseded == 1 && hs.presents == 1 && h.nwait == 2 && h.clock >= 3 * MS, "result %d superseded %llu presents %llu waits %d (G held to commit+H, then C held to its own commit+H)", r, (unsigned long long)hs.superseded, (unsigned long long)hs.presents, h.nwait);
        r = hold_frame(1, 3, 0, 0, &h, &hs);
        CHECK("hold: G only -> G presented at commit+H", r == 1 && hs.superseded == 0 && h.nwait == 1 && h.clock == 3 * MS + 1 && h.waited_sum == 3 * MS + 1 - 3 * MS / 2, "result %d clock %llu ns waited %llu ns", r, (unsigned long long)h.clock, (unsigned long long)h.waited_sum);
        r = hold_frame(1, 3, 1, 5 * MS, &h, &hs);
        CHECK("hold: C after H -> both presented", r == 3 && hs.superseded == 0 && hs.presents == 2, "result %d presents %llu", r, (unsigned long long)hs.presents);
        r = hold_frame(1, 8, 1, 5 * MS, &h, &hs);
        CHECK("hold: H=8 widens the window", r == 2 && hs.superseded == 1, "result %d (C at 5 ms inside 8 ms)", r);
        n48df_t base; hsim_t hb; r = hold_frame(0, 3, 1, 2580000ull, &hb, &base);
        CHECK("hold OFF: identical to today (no wait, G presented before C exists)", r == 3 && hb.nwait == 0 && base.superseded == 0 && base.presents == 2, "result %d waits %d", r, hb.nwait);
        // OFF identity: the same event stream through the plain complete_surf gives the same state
        n48df_t p = active(); uint32_t all3[3] = { R, R, R }; int a1 = n48df_pick(&p, all3), a2 = n48df_pick(&p, all3);
        n48df_submit(&p, a1, 7, 1); int g = n48df_complete_surf(&p, a1, 0, 1, 7); if (g) n48df_present_result(&p, 0);
        n48df_submit(&p, a2, 7, 2); int c = n48df_complete_surf(&p, a2, 0, 2, 7); if (c) n48df_present_result(&p, 0);
        CHECK("hold OFF: state equals plain complete_surf", g && c && p.presents == base.presents && p.superseded == base.superseded && p.last_seq == base.last_seq && p.stale == base.stale, "presents %llu superseded %llu last_seq %llu", (unsigned long long)p.presents, (unsigned long long)p.superseded, (unsigned long long)p.last_seq);
        // bound: never more than H, whatever the clocks say
        int boundok = 1;
        for (int hm = 1; hm <= 8; hm++) for (uint64_t tcm = 0; tcm < 40; tcm++) for (uint64_t nw = 0; nw < 40; nw += 3) { uint64_t w = n48df_hold_ns(1, hm, tcm * MS / 2 + 1, nw * MS / 2); if (w > (uint64_t)hm * MS) boundok = 0; }
        CHECK("hold: wait never exceeds H (incl. commit time in the future)", boundok && n48df_hold_ns(1, 3, 100 * MS, 1 * MS) == 3 * MS && n48df_hold_ns(1, 3, 1 * MS, 10 * MS) == 0 && n48df_hold_ns(1, 3, 0, 1) == 0 && n48df_hold_ns(0, 3, 1 * MS, 1 * MS) == 0, "future commit gives exactly H, past deadline 0, unknown commit 0, off 0");
        // not held when already superseded, failed, or stale
        n48df_t q = active(); int q1 = n48df_pick(&q, all3), q1b = n48df_pick(&q, all3); hsim_t hq; memset(&hq, 0, sizeof hq); hq.s = &q; uint64_t wq;
        n48df_submit(&q, q1, 7, 1); n48df_submit(&q, q1b, 7, 2);
        int rq = n48df_complete_held(&q, q1, 0, 1, 7, 1, 3, 1, 2 * MS, hsim_wait, &hq, &wq);
        CHECK("hold: already superseded -> no wait, skipped at once", !rq && hq.nwait == 0 && wq == 0 && q.superseded == 1, "waits %d superseded %llu", hq.nwait, (unsigned long long)q.superseded);
        n48df_t q2 = active(); int qa = n48df_pick(&q2, all3); hsim_t h2; memset(&h2, 0, sizeof h2); h2.s = &q2; n48df_submit(&q2, qa, 7, 1);
        int r2 = n48df_complete_held(&q2, qa, -4, 1, 7, 1, 3, 1, 2 * MS, hsim_wait, &h2, &wq);
        CHECK("hold: GPU failure -> no wait", !r2 && h2.nwait == 0 && q2.gpu_fail == 1, "waits %d", h2.nwait);
        int r3 = n48df_complete_held(&q2, 0, 0, 1, 0, 1, 3, 1, 2 * MS, hsim_wait, &h2, &wq);
        CHECK("hold: unknown surface (sid 0) / free slot -> no wait", h2.nwait == 0 && !r3, "waits %d", h2.nwait);
        // parse table
        CHECK("hold: H parse", n48df_hold_parse("5\n", 2) == 5 && n48df_hold_parse("1", 1) == 1 && n48df_hold_parse("8", 1) == 8 && n48df_hold_parse("", 0) == 3 && n48df_hold_parse("0", 1) == 3 && n48df_hold_parse("9", 1) == 3 &&
              n48df_hold_parse("abc", 3) == 3 && n48df_hold_parse("-2", 2) == 3 && n48df_hold_parse("2.5", 3) == 3 && n48df_hold_parse(" 4 \n", 4) == 4 && n48df_hold_parse("12", 2) == 3 && n48df_hold_parse("3x", 2) == 3 && n48df_hold_parse("007", 3) == 7, "1..8 -> H, else 3");
        // the soak: the presented order stays the submission order and nothing is presented twice, with random G/C gaps
        srand(777); uint64_t viol = 0, tot = 0;
        for (int i = 0; i < 20000; i++) { int on = rand() & 1, hm = 1 + rand() % 8, wc = rand() % 3 != 0; uint64_t tc = (uint64_t)(rand() % 12000) * 1000ull + 1; r = hold_frame(on, hm, wc, tc, &h, &hs); tot++;
            if (hs.presents != (uint64_t)((r & 1) + ((r >> 1) & 1)) || hs.last_seq > 2 || h.waited_max > (uint64_t)hm * MS || (r == 0) || (wc && !(r & 2))) viol++;
            if (on && wc && (tc <= (uint64_t)hm * MS) && (r & 1)) viol++;   // C inside the window must have superseded G
            if (!on && h.nwait) viol++; }
        CHECK("hold soak 20000 frames: C always presents, C inside H always supersedes G, wait <= H, off never waits", viol == 0, "%llu violations in %llu", (unsigned long long)viol, (unsigned long long)tot);
    }

    // ---- P6: protected display surfaces (image source) ----
    {
        typedef n48df_rect_t RC; n48df_imgreg_t g; const uint32_t P = 10240;
        CHECK("img: full 2560x1440 BGRA at pitch 10240", n48df_img_region((RC){ 0, 0, 2560, 1440 }, 2560, 1440, P, 4, &g) && g.buf_off == 0 && g.row_len == 2560 && g.x == 0 && g.y == 0 && g.w == 2560 && g.h == 1440, "off %llu rowlen %u %ux%u", (unsigned long long)g.buf_off, g.row_len, g.w, g.h);
        CHECK("img: partial rect lands at the buffer path's byte offset (y*pitch + x*4)", n48df_img_region((RC){ 10, 5, 30, 8 }, 2560, 1440, P, 4, &g) && g.buf_off == 5ull * P + 40 && g.x == 10 && g.y == 5 && g.w == 20 && g.h == 3 && g.row_len == 2560, "off %llu", (unsigned long long)g.buf_off);
        n48df_region_t rg[4]; uint32_t n = n48df_regions((RC){ 0, 100, 2560, 300 }, 2560, 1440, P, 4, rg, 4);
        CHECK("img: full-width rows equal the buffer path's single region", n == 1 && n48df_img_region((RC){ 0, 100, 2560, 300 }, 2560, 1440, P, 4, &g) && g.buf_off == rg[0].src && (uint64_t)g.h * P == rg[0].size, "off %llu size %llu", (unsigned long long)g.buf_off, (unsigned long long)rg[0].size);
        CHECK("img: clipped to the surface", n48df_img_region((RC){ -20, -20, 8, 3 }, 2560, 1440, P, 4, &g) && g.x == 0 && g.y == 0 && g.w == 8 && g.h == 3 && g.buf_off == 0, "%ux%u", g.w, g.h);
        CHECK("img: empty / off-surface = no copy", !n48df_img_region((RC){ 5, 5, 5, 9 }, 2560, 1440, P, 4, &g) && !n48df_img_region((RC){ 3000, 0, 3100, 10 }, 2560, 1440, P, 4, &g) && !n48df_img_region((RC){ 0, 1440, 100, 1500 }, 2560, 1440, P, 4, &g), "0");
        CHECK("img: pitch not a whole number of texels, narrower than a row, bpp 0: refused", !n48df_img_ok(2560, 1440, 10242, 4) && !n48df_img_ok(2560, 1440, 8192, 4) && !n48df_img_ok(2560, 1440, 10240, 0) && n48df_img_ok(2560, 1440, 10240, 4) && !n48df_img_region((RC){ 0, 0, 10, 10 }, 2560, 1440, 10242, 4, &g), "refused");
        int agree = 1; srand(4242);   // the image regions cover exactly the bytes the buffer-path regions cover
        for (int i = 0; i < 20000; i++) {
            uint32_t W = 16 + (uint32_t)rand() % 200, H = 8 + (uint32_t)rand() % 60, PP = W * 4 + 4 * ((uint32_t)rand() % 8); RC q = { rand() % 260 - 30, rand() % 90 - 15, rand() % 260 - 30, rand() % 90 - 15 };
            n48df_region_t rr[128]; uint32_t nn = n48df_regions(q, W, H, PP, 4, rr, 128); int ok = n48df_img_region(q, W, H, PP, 4, &g);
            if ((nn == 0) != !ok) { agree = 0; break; }
            if (!ok) continue;
            uint64_t bytes = 0; for (uint32_t k = 0; k < nn; k++) bytes += rr[k].size;
            uint64_t span = nn == 1 && rr[0].size == (uint64_t)g.h * PP ? 0 : 1;   // single contiguous region: whole rows
            if (g.buf_off != rr[0].src || (span == 0 ? bytes != (uint64_t)g.h * PP : bytes != (uint64_t)g.w * 4 * g.h) || g.row_len != PP / 4) { agree = 0; break; }
        }
        CHECK("img: 20000 random rects agree with n48df_regions (offset, byte count, row length)", agree, "agree");
        n48cc cc; memset(&cc, 0, sizeof cc); int cv = 0;
        n48cc_put(&cc, 376, 0, 2560, 1440, P, n48df_nobase_alloc(0, P, 1440), 1);
        CHECK("classify cache: base-less key (base 0) hits for itself", n48cc_lookup(&cc, 376, 0, 2560, 1440, P, n48df_nobase_alloc(0, P, 1440), &cv) && cv == 1, "hit");
        CHECK("classify cache: a mapped surface of the same id (real base) does not take the base-less verdict", !n48cc_lookup(&cc, 376, 0x7f0000000000ull, 2560, 1440, P, 14745600ull, &cv), "miss");
        CHECK("nobase alloc: reported size kept, 0 -> bpr*h", n48df_nobase_alloc(14745600, P, 1440) == 14745600 && n48df_nobase_alloc(0, P, 1440) == 14745600ull, "%llu", (unsigned long long)n48df_nobase_alloc(0, P, 1440));
    }

    // ---- bundle 12 (M6, queue 290 S3): n48df_skip_content - a frame whose copy is another display's content is released without a present and WITHOUT touching last_seq ----
    {
        uint32_t all[3] = { R, R, R };
        n48df_t k = active(); int a = n48df_pick(&k, all), b = n48df_pick(&k, all);
        k.fid[a] = 10; k.fid[b] = 11; k.head = b; k.last_seq = 3; k.presents = 2;
        n48df_skip_content(&k, a);
        CHECK("skip: the slot is released, nothing presented, last_seq untouched, no GPU failure counted", k.inflight[a] == 0 && k.fid[a] == 0 && k.presents == 2 && k.last_seq == 3 && k.gpu_fail == 0 && k.m6_skip == 1 && k.stale == 0, "m6_skip %llu last_seq %llu", (unsigned long long)k.m6_skip, (unsigned long long)k.last_seq);
        CHECK("skip: the chain restarts (head -1) and the LATER chained frame still in flight is poisoned (it read this slot as its base)", k.head == -1 && k.poison[b] == 1 && k.inflight[b] == 1, "head %d poison[b] %d", k.head, k.poison[b]);
        CHECK("skip: the poisoned later frame is never presented", n48df_complete_seq(&k, b, 0, 5) == 0 && k.chain_invalid == 1, "chain_invalid %llu", (unsigned long long)k.chain_invalid);
        n48df_skip_content(&k, a); n48df_skip_content(&k, -1); n48df_skip_content(&k, N48DF_MAX_SLOTS);
        CHECK("skip: a slot that is not in flight, and out-of-range slots, change nothing", k.m6_skip == 1 && k.last_seq == 3, "m6_skip %llu", (unsigned long long)k.m6_skip);
        n48df_t o = active(); int x = n48df_pick(&o, all), y = n48df_pick(&o, all);
        n48df_skip_content(&o, x);                                           // seq 9 is another display's frame: skipped
        CHECK("skip: an OLDER real frame completing afterwards is still PRESENTED (a skipped frame never made it stale; complete_held would have moved last_seq to 9)", n48df_complete_seq(&o, y, 0, 4) == 1 && o.last_seq == 4 && o.stale == 0, "last_seq %llu stale %llu", (unsigned long long)o.last_seq, (unsigned long long)o.stale);
        n48df_t q = active(); int u = n48df_pick(&q, all); n48df_skip_content(&q, u); int u2 = n48df_pick(&q, all);
        CHECK("skip: the released slot is free for the next frame at once", u2 >= 0 && q.m6_skip == 1, "picked %d", u2);
    }
    // ---- bundle 20: HDMI rubberband fix (an internal design note Q3): F1 known scanout surfaces, F2 per-slot supersede marks, F3 re-copy with the noted seq, F4 counters ----
    {
        const uint32_t PS = N48DF_W_PASS, DFN = N48DF_W_DRAW_FINAL, DOT = N48DF_W_DRAW_OTHER, CMP = N48DF_W_DRAW_COMP;
        const uint32_t G_ = PS | DFN, C_ = PS | DOT | CMP;
        // F1: the table of 8 proxy flags is full; a 9th surface
        n48df_t a = active(); for (uint32_t k = 1; k <= 8; k++) n48df_account_sid(&a, G_, 100 + k, 0);
        CHECK("F1: 8 proxy flags set, the ninth is refused and counted", a.nscan == 8 && n48df_account_sid(&a, G_, 209, 0) == 1 && !n48df_is_scan(&a, 209) && a.scan_full_refused == 1, "scan_full_refused %llu", (unsigned long long)a.scan_full_refused);
        CHECK("F1: before the fix a composite into the 9th surface was refused (tentative / latch-off frame keeps the proxy rule)", n48df_account_sid(&a, C_, 209, 0) == 0 && a.nonfinal[N48DF_C_RENDER_COMP] == 1, "refused");
        CHECK("F1: known (latch on, planned non-tentatively): the composite presents with no table entry", n48df_account_sid(&a, C_, 209, 1) == 1 && a.nscan == 8 && !n48df_is_scan(&a, 209), "presented");
        // the G+C pair into the 9th surface: C presents and supersedes G
        int sg = n48df_pick(&a, all), sc = n48df_pick(&a, all);
        n48df_submit(&a, sg, 209, 11); n48df_submit(&a, sc, 209, 12);
        CHECK("F1+F2: 9th surface G+C pair: G skipped as superseded, C presents", n48df_complete_surf(&a, sg, 0, 11, 209) == 0 && a.superseded == 1 && n48df_complete_surf(&a, sc, 0, 12, 209) == 1 && a.last_seq == 12, "superseded %llu last_seq %llu", (unsigned long long)a.superseded, (unsigned long long)a.last_seq);
        // unknown / ambiguous stay non-presentable: sid 0 never presents as a composite even when known, an unflagged composite without known is refused, ColorFill/blit never
        n48df_t u = active();
        CHECK("F1: sid 0 composite stays non-presentable even when known=1", n48df_account_sid(&u, C_, 0, 1) == 0 && u.nonfinal[N48DF_C_RENDER_COMP] == 1, "refused");
        CHECK("F1: fill-only / blit into a known surface stay non-final", n48df_account_sid(&u, PS | DOT | N48DF_W_DRAW_FILL, 5, 1) == 0 && n48df_account_sid(&u, PS | N48DF_W_BLIT, 5, 1) == 0, "refused");
        CHECK("F1: known does not set the proxy flag table (nothing to saturate)", n48df_account_sid(&u, G_, 6, 1) == 1 && u.nscan == 0 && u.nflag_log == 0 && u.scan_full_refused == 0, "nscan 0");
        CHECK("F1: known=0 keeps the proxy flag (tentative / latch-off frames)", n48df_account_sid(&u, G_, 7, 0) == 1 && u.nscan == 1 && n48df_account_sid(&u, C_, 7, 0) == 1, "flag set, composite follows");
        // 200 rearrangement cycles of 4 new IDs each (800 distinct surfaces): bounded state, every G whose C was submitted first is skipped
        n48df_t r = active(); uint64_t seq = 0, viol = 0, wantp = 0; uint32_t id = 1000;
        for (int cyc = 0; cyc < 200; cyc++) {
            for (int k = 0; k < 4; k++) {
                id++;
                int okg = n48df_account_sid(&r, G_, id, 1), g = n48df_pick(&r, all); n48df_submit(&r, g, id, ++seq);
                int okc = n48df_account_sid(&r, C_, id, 1), c = n48df_pick(&r, all); n48df_submit(&r, c, id, ++seq);     // C submitted BEFORE G's completion is handled
                if (!okg || !okc || g < 0 || c < 0) viol++;
                if (n48df_complete_surf(&r, g, 0, seq - 1, id) != 0) viol++;                       // G is skipped
                if (n48df_complete_surf(&r, c, 0, seq, id) != 1) viol++; else { n48df_present_result(&r, 0); wantp++; }
                for (int j = 0; j < 3; j++) if (r.inflight[j] || r.sup_n[j] || r.made[j]) viol++;
            }
        }
        CHECK("F2: 200 rearrangement cycles x 4 new IDs: state stays bounded and every G is skipped", viol == 0 && r.nscan == 0 && r.scan_full_refused == 0 && r.surf_untracked == 0 && r.superseded == 800 && r.presents == wantp && r.stale == 0 && r.state == N48DF_ACTIVE, "%llu violations, superseded %llu presents %llu", (unsigned long long)viol, (unsigned long long)r.superseded, (unsigned long long)r.presents);
        // an ID reused with an OLD seq
        n48df_t o = active(); int x0 = n48df_pick(&o, all), x1 = n48df_pick(&o, all);
        n48df_submit(&o, x0, 5, 10); n48df_submit(&o, x1, 5, 7);                                    // the same ID, a lower seq submitted after a higher one
        CHECK("F2: a reused ID with an old seq is itself superseded; the newer one is not", n48df_is_superseded(&o, x1) && !n48df_is_superseded(&o, x0) && n48df_complete_surf(&o, x1, 0, 7, 5) == 0 && o.superseded == 1 && n48df_complete_surf(&o, x0, 0, 10, 5) == 1, "seq 7 skipped, seq 10 presents");
        int y0 = n48df_pick(&o, all); n48df_submit(&o, y0, 5, 3);                                   // ID 5 again, nothing in flight: no mark; seq 3 is just stale
        CHECK("F2: a reused ID with nothing in flight carries no old mark (stale, not superseded)", !n48df_is_superseded(&o, y0) && n48df_complete_surf(&o, y0, 0, 3, 5) == 0 && o.stale == 1 && o.superseded == 1, "stale %llu", (unsigned long long)o.stale);
        // a failed later frame un-supersedes the earlier one; with two superseders one failure leaves it superseded
        n48df_t f = active(); int e0 = n48df_pick(&f, all), e1 = n48df_pick(&f, all), e2 = n48df_pick(&f, all);
        n48df_submit(&f, e0, 8, 1); n48df_submit(&f, e1, 8, 2); n48df_submit(&f, e2, 8, 3);
        CHECK("F2: two later frames: seq 1 carries two marks (bounded by the slots), seq 2 one", f.sup_n[e0] == 2 && f.sup_n[e1] == 1 && f.sup_n[e2] == 0, "marks %u %u %u", f.sup_n[e0], f.sup_n[e1], f.sup_n[e2]);
        CHECK("F2: the newest fails: seq 2 is superseded by nothing, seq 1 still by seq 2", n48df_complete_surf(&f, e2, -3, 3, 8) == 0 && f.sup_n[e0] == 1 && f.sup_n[e1] == 0 && f.gpu_fail == 1, "gpu_fail 1");
        CHECK("F2: seq 2 presents (its superseder failed), seq 1 is never shown after it", n48df_complete_surf(&f, e1, 0, 2, 8) == 1 && n48df_complete_surf(&f, e0, 0, 1, 8) == 0 && f.stale == 1 && f.superseded == 0, "2 shown, 1 not shown (stale: a newer frame was presented first)");
        n48df_t g = active(); int h0 = n48df_pick(&g, all), h1 = n48df_pick(&g, all); n48df_submit(&g, h0, 9, 1); n48df_submit(&g, h1, 9, 2);
        CHECK("F2: the only superseder fails: the earlier frame presents", n48df_complete_surf(&g, h1, -3, 2, 9) == 0 && !n48df_is_superseded(&g, h0) && n48df_complete_surf(&g, h0, 0, 1, 9) == 1 && g.superseded == 0, "seq 1 presented");
        // a slot reused for a new frame carries none of the old frame's marks
        n48df_t m = active(); int i0 = n48df_pick(&m, all), i1 = n48df_pick(&m, all); n48df_submit(&m, i0, 4, 1); n48df_submit(&m, i1, 4, 2);
        (void)n48df_complete_surf(&m, i0, 0, 1, 4); int i2 = n48df_pick(&m, all);   // i0 (skipped) is free again; i2 may be it
        CHECK("F2: a released slot has no marks left", !n48df_is_superseded(&m, i0) && m.sup_n[i0] == 0 && m.made[i0] == 0 && i2 >= 0, "clean");
        n48df_submit(&m, i2, 4, 3); (void)n48df_complete_surf(&m, i1, -3, 2, 4);     // seq 2 fails after seq 3 reused a slot: seq 3 has no marks to undo
        CHECK("F2: marks never go negative", m.sup_n[i2] == 0 && m.sup_n[i1] == 0 && n48df_complete_surf(&m, i2, 0, 3, 4) == 1, "seq 3 presents");
        // F2: a mark taken back by a failing superseder must never land on a LATER frame that reuses the superseded slot
        n48df_t ru = active(); int sa = n48df_pick(&ru, all), sb = n48df_pick(&ru, all), sc2 = n48df_pick(&ru, all);
        n48df_submit(&ru, sa, 20, 1); n48df_submit(&ru, sb, 20, 2);                      // seq 1 (slot sa) superseded by seq 2 (slot sb)
        (void)n48df_complete_surf(&ru, sa, 0, 1, 20);                                   // seq 1 skipped, slot sa free again
        int sd = n48df_pick(&ru, all); n48df_submit(&ru, sd, 30, 3); n48df_submit(&ru, sc2, 30, 4);   // a new frame (seq 3, another surface) reuses a slot and is superseded by seq 4
        CHECK("F2: the reused slot starts clean and is marked only by its own superseder", sd == sa && ru.sup_n[sd] == 1 && ru.made[sb] == 0 && ru.made[sc2] == (uint8_t)(1u << sd), "slot %d marks %u", sd, ru.sup_n[sd]);
        (void)n48df_complete_surf(&ru, sb, -3, 2, 20);                                   // seq 2 fails LATER: it made no mark on the new frame
        CHECK("F2: a later failure of the old superseder does not un-supersede the new frame", ru.sup_n[sd] == 1 && n48df_complete_surf(&ru, sd, 0, 3, 30) == 0 && ru.superseded == 2, "seq 3 still skipped");
        // F3: a re-copy presents with the NOTED frame's seq: after a newer present it is stale and not shown; with a fresh newest seq it would have been (the bug)
        n48df_t rc = active(); int p0 = n48df_pick(&rc, all); n48df_submit(&rc, p0, 6, 5); int sh = n48df_complete_surf(&rc, p0, 0, 5, 6); if (sh) n48df_present_result(&rc, 0);
        int rs = n48df_pick(&rc, all);
        CHECK("F3: a re-copy noted at seq 3, after seq 5 was presented, is stale and NOT shown", sh && n48df_complete_seq(&rc, rs, 0, 3) == 0 && rc.stale == 1 && rc.last_seq == 5, "stale 1");
        int rs2 = n48df_pick(&rc, all);
        CHECK("F3: a re-copy noted at seq 8 with nothing newer is shown", n48df_complete_seq(&rc, rs2, 0, 8) == 1 && rc.last_seq == 8, "shown");
        // F4: occupancy helpers
        n48df_t oc = active(); int q0 = n48df_pick(&oc, all), q1 = n48df_pick(&oc, all); n48df_submit(&oc, q0, 3, 1); n48df_submit(&oc, q1, 3, 2);
        n48df_account_sid(&oc, C_, 77, 0);
        CHECK("F4: occupancy helpers", n48df_marked_count(&oc) == 1 && n48df_nonscan_used(&oc) == 1, "marked %d nonscan %d", n48df_marked_count(&oc), n48df_nonscan_used(&oc));
    }

    // ---- soak: random flags and completion order; invariants ----
    srand(12345);
    n48df_t z = active(); int held[3] = { 0, 0, 0 }; uint64_t presented = 0, bad = 0;
    for (int i = 0; i < 200000; i++) {
        uint32_t fl[3]; for (int k = 0; k < 3; k++) fl[k] = (rand() % 3) ? R : (uint32_t)(rand() & 3);
        int pk = n48df_pick(&z, fl);
        if (pk >= 0) { if (held[pk] || !(fl[pk] & R)) bad++; held[pk] = 1; }
        int cs = rand() % 3;
        if (held[cs] && (rand() & 1)) { int fail = (rand() % 11) == 0; int want = n48df_complete(&z, cs, fail ? -3 : 0); held[cs] = 0; if (want) { presented++; if (n48df_present_result(&z, 0)) bad++; } else if (!fail) bad++; }
        for (int k = 0; k < 3; k++) if (held[k] != z.inflight[k]) bad++;
    }
    CHECK("soak 200000 steps", bad == 0 && z.presents == presented && z.state == N48DF_ACTIVE, "%llu violations, %llu presents, %llu drops, %llu gpu failures", (unsigned long long)bad, (unsigned long long)z.presents, (unsigned long long)z.drops, (unsigned long long)z.gpu_fail);
    printf(fails ? "FAIL test-dispflip (%d)\n" : "PASS test-dispflip\n", fails);
    return fails ? 1 : 0;
}
