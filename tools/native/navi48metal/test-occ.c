// test-occ.c: host test of n48_occ.h (P2, build 18) plus source pins on Navi48Device.m for the glue.
// Build/run (from this directory):  cc -O1 -Wall -Wextra -Werror -o /tmp/test-occ test-occ.c && /tmp/test-occ .      (argv[1] = the directory holding Navi48Device.m, default ".")
//
// The first half REPLAYS THE MEASUREMENTS made on the host Mac's real Apple Metal (test-occlusion-semantics.m, experiments E1..E9) through a tiny executor that does what N48RenderEncoder does with the
// decisions (set -> maybe end the query; draw -> maybe begin a query and count samples into the newest open interval; pass end -> end the query; resolve -> n48occ_final per used offset into the buffer),
// and demands Apple's numbers. The second half is the fail-safe and the arithmetic; the third half pins the glue in Navi48Device.m (ordering and reachability, not just text).
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "n48_occ.h"
static int fails;
#define CHECK(name, cond, ...) do { int ok_ = (cond); printf("%s %s: ", ok_ ? "ok  " : "FAIL", name); printf(__VA_ARGS__); printf("\n"); if (!ok_) fails++; } while (0)

// ---- the executor ----
typedef struct { n48occ o; uint64_t res[N48OCC_MAXQ]; int open; int begun, ended; } pass;
static void p_begin(pass *p, uint64_t buflen, int acc) { memset(p, 0, sizeof *p); n48occ_init(&p->o, buflen, acc); }
static void p_set(pass *p, unsigned mode, uint64_t off) { int e; (void)n48occ_set(&p->o, mode, off, &e); if (e) { if (!p->open) { printf("BUG: end without open\n"); fails++; } p->open = 0; p->ended++; } }
static void p_draw(pass *p, uint64_t samples) {
    uint32_t q; int pr;
    if (n48occ_draw(&p->o, &q, &pr)) { p->open = 1; p->begun++; p->res[q] = 0; }
    if (p->open) p->res[p->o.activeQ] += samples;   // samples land in the interval that is open at the time of the draw
}
static void p_split(pass *p) { uint32_t q; if (n48occ_end(&p->o, &q)) { p->open = 0; p->ended++; } }   // n48EndPass
static void p_resolve(pass *p, uint64_t *buf, int ok) {
    p_split(p);
    uint64_t old[N48OCC_MAXU]; for (unsigned u = 0; u < p->o.nu; u++) old[u] = buf[p->o.u[u].off / 8];
    for (unsigned u = 0; u < p->o.nu; u++) buf[p->o.u[u].off / 8] = n48occ_final(&p->o, u, ok ? p->res : NULL, ok, old[u]);
}
#define SENT 0xABABABABABABABABull

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : ".";
    uint64_t b[16]; pass p;
    // E1: one pass, Reset, sentinel everywhere
    for (int i = 0; i < 16; i++) b[i] = SENT;
    p_begin(&p, 128, 0);
    p_set(&p, N48OCC_COUNTING, 0);  p_draw(&p, 4096); p_draw(&p, 4096);
    p_set(&p, N48OCC_BOOLEAN, 8);   p_draw(&p, 4096);
    p_set(&p, N48OCC_BOOLEAN, 16);  p_draw(&p, 0);
    p_set(&p, N48OCC_COUNTING, 24);
    p_set(&p, N48OCC_COUNTING, 32); p_draw(&p, 16);
    p_set(&p, N48OCC_DISABLED, 0);  p_draw(&p, 4096);                     // a disabled draw adds nothing
    p_set(&p, N48OCC_COUNTING, 0);  p_draw(&p, 4096);                     // back to offset 0: the interval adds to the earlier ones
    p_set(&p, N48OCC_BOOLEAN, 40);  p_draw(&p, 0); p_draw(&p, 4096);     // hidden then visible
    p_resolve(&p, b, 1);
    CHECK("E1 counting sum across draws and across intervals of one offset (S1)", b[0] == 12288, "[0]=%llu (Apple: 12288)", (unsigned long long)b[0]);
    CHECK("E1 boolean visible writes exactly 1 (S2)", b[1] == 1, "[8]=%llu (Apple: 1)", (unsigned long long)b[1]);
    CHECK("E1 boolean hidden writes 0", b[2] == 0, "[16]=%llu (Apple: 0)", (unsigned long long)b[2]);
    CHECK("E1 an offset selected with no draw is WRITTEN with 0 (S3)", b[3] == 0, "[24]=%llu (Apple: 0)", (unsigned long long)b[3]);
    CHECK("E1 a scissored 4x4 draw counts 16", b[4] == 16, "[32]=%llu (Apple: 16)", (unsigned long long)b[4]);
    CHECK("E1 boolean: hidden draw then visible draw -> 1", b[5] == 1, "[40]=%llu (Apple: 1)", (unsigned long long)b[5]);
    CHECK("E1 offsets never selected stay untouched (S3)", b[6] == SENT && b[7] == SENT && b[8] == SENT, "[48]=%llx [56]=%llx", (unsigned long long)b[6], (unsigned long long)b[7]);
    CHECK("E1 every opened query was closed exactly once", p.begun == p.ended && !p.open, "begun %d ended %d", p.begun, p.ended);
    // E2/E5: Reset overwrites, second pass offscreen -> 0 ; Accumulate adds
    for (int i = 0; i < 16; i++) b[i] = SENT;
    p_begin(&p, 64, 0); p_set(&p, N48OCC_COUNTING, 0); p_draw(&p, 4096); p_resolve(&p, b, 1);
    CHECK("E2 Reset pass 0: 4096 over the sentinel (S4)", b[0] == 4096 && b[1] == SENT, "[0]=%llu [8]=%llx", (unsigned long long)b[0], (unsigned long long)b[1]);
    p_begin(&p, 64, 0); p_set(&p, N48OCC_COUNTING, 0); p_draw(&p, 0); p_resolve(&p, b, 1);
    CHECK("E2 Reset pass 1 (nothing visible) overwrites with 0", b[0] == 0, "[0]=%llu (Apple: 0)", (unsigned long long)b[0]);
    for (int i = 0; i < 16; i++) b[i] = SENT;
    p_begin(&p, 64, 1); p_set(&p, N48OCC_COUNTING, 0); p_draw(&p, 4096); p_resolve(&p, b, 1);
    p_begin(&p, 64, 1); p_set(&p, N48OCC_COUNTING, 0); p_draw(&p, 0); p_resolve(&p, b, 1);
    CHECK("E2 Accumulate: old + 4096, then old + 0 (S5)", b[0] == SENT + 4096, "[0]=%llx (Apple: ...115179 = sentinel + 4096)", (unsigned long long)b[0]);
    for (int i = 0; i < 16; i++) b[i] = SENT;
    p_begin(&p, 64, 0); p_set(&p, N48OCC_COUNTING, 0); p_draw(&p, 4096); p_resolve(&p, b, 1);
    p_begin(&p, 64, 0); p_set(&p, N48OCC_COUNTING, 0); p_draw(&p, 4096); p_resolve(&p, b, 1);
    CHECK("E5 two Reset passes: 4096, not 8192", b[0] == 4096, "[0]=%llu (Apple: 4096)", (unsigned long long)b[0]);
    for (int i = 0; i < 16; i++) b[i] = SENT;
    p_begin(&p, 64, 1); p_set(&p, N48OCC_COUNTING, 0); p_draw(&p, 4096); p_resolve(&p, b, 1);
    p_begin(&p, 64, 1); p_set(&p, N48OCC_COUNTING, 0); p_draw(&p, 4096); p_resolve(&p, b, 1);
    CHECK("E5 two Accumulate passes: old + 8192", b[0] == SENT + 8192, "[0]=%llx", (unsigned long long)b[0]);
    // E3: no mode ever set -> untouched
    for (int i = 0; i < 16; i++) b[i] = SENT;
    p_begin(&p, 64, 0); p_draw(&p, 4096); p_resolve(&p, b, 1);
    CHECK("E3 a pass that never selects a mode touches nothing", b[0] == SENT && p.begun == 0, "[0]=%llx begun %d", (unsigned long long)b[0], p.begun);
    // E6/E9: Accumulate + Boolean ORs
    memset(b, 0, sizeof b);
    p_begin(&p, 64, 1); p_set(&p, N48OCC_BOOLEAN, 0); p_draw(&p, 4096); p_resolve(&p, b, 1);
    p_begin(&p, 64, 1); p_set(&p, N48OCC_BOOLEAN, 0); p_draw(&p, 4096); p_resolve(&p, b, 1);
    CHECK("E6 Accumulate boolean over two passes stays 1", b[0] == 1, "[0]=%llu (Apple: 1)", (unsigned long long)b[0]);
    for (int i = 0; i < 16; i++) b[i] = 4;
    p_begin(&p, 64, 1); p_set(&p, N48OCC_BOOLEAN, 0); p_draw(&p, 4096); p_set(&p, N48OCC_BOOLEAN, 8); p_draw(&p, 0); p_set(&p, N48OCC_COUNTING, 16); p_draw(&p, 0); p_resolve(&p, b, 1);
    CHECK("E9 Accumulate preset 4: boolean visible 4|1 = 5, hidden stays 4, counting hidden stays 4", b[0] == 5 && b[1] == 4 && b[2] == 4, "[0]=%llu [8]=%llu [16]=%llu (Apple: 5 4 4)", (unsigned long long)b[0], (unsigned long long)b[1], (unsigned long long)b[2]);
    // E7: boolean revisit with nothing visible keeps 1
    for (int i = 0; i < 16; i++) b[i] = 0xFFFFFFFFFFFFFFFFull;
    p_begin(&p, 64, 0); p_set(&p, N48OCC_BOOLEAN, 0); p_draw(&p, 4096); p_draw(&p, 4096); p_set(&p, N48OCC_BOOLEAN, 8); p_draw(&p, 4096); p_set(&p, N48OCC_BOOLEAN, 0); p_draw(&p, 0); p_resolve(&p, b, 1);
    CHECK("E7 boolean offset revisited with nothing visible stays 1", b[0] == 1 && b[1] == 1 && b[2] == 0xFFFFFFFFFFFFFFFFull, "[0]=%llu [8]=%llu [16]=%llx", (unsigned long long)b[0], (unsigned long long)b[1], (unsigned long long)b[2]);

    // ---- pass splits: a Vulkan render pass ends mid-interval (layout change, barrier, upload); the query ends there and a new one starts at the next draw; the sum is unchanged
    memset(b, 0, sizeof b);
    p_begin(&p, 64, 0); p_set(&p, N48OCC_COUNTING, 0); p_draw(&p, 100); p_split(&p); p_draw(&p, 200); p_split(&p); p_split(&p); p_draw(&p, 300); p_resolve(&p, b, 1);
    CHECK("split: 3 intervals of one offset sum to 600", b[0] == 600 && p.begun == 3 && p.ended == 3, "[0]=%llu begun %d ended %d", (unsigned long long)b[0], p.begun, p.ended);
    p_begin(&p, 64, 0); p_set(&p, N48OCC_BOOLEAN, 0); p_draw(&p, 0); p_split(&p); p_draw(&p, 5); p_resolve(&p, b, 1);
    CHECK("split: boolean hidden before the split, visible after -> 1", b[0] == 1, "[0]=%llu", (unsigned long long)b[0]);
    p_begin(&p, 64, 0); p_set(&p, N48OCC_COUNTING, 0); p_draw(&p, 7); p_split(&p); p_set(&p, N48OCC_COUNTING, 8); p_draw(&p, 9); p_resolve(&p, b, 1);
    CHECK("split then offset change: no end of a query that is already closed", b[0] == 7 && b[1] == 9 && p.begun == 2 && p.ended == 2, "[0]=%llu [8]=%llu begun %d ended %d", (unsigned long long)b[0], (unsigned long long)b[1], p.begun, p.ended);

    // ---- validation: a rejected call is Disabled, never a wrong write
    memset(b, 0, sizeof b); b[1] = 77;
    p_begin(&p, 64, 0); { int e; int rc = n48occ_set(&p.o, N48OCC_COUNTING, 12, &e); CHECK("offset 12 (not a multiple of 8) is rejected", rc == -1 && p.o.mode == N48OCC_DISABLED, "rc %d mode %d", rc, p.o.mode); }
    { int e; int rc = n48occ_set(&p.o, N48OCC_COUNTING, 64, &e); CHECK("offset 64 in a 64-byte buffer is rejected", rc == -1, "rc %d", rc); }
    { int e; int rc = n48occ_set(&p.o, N48OCC_COUNTING, 56, &e); CHECK("offset 56 in a 64-byte buffer is accepted", rc == 0 && p.o.mode == N48OCC_COUNTING, "rc %d", rc); }
    { int e; int rc = n48occ_set(&p.o, 7, 0, &e); CHECK("mode 7 is rejected and ends the open selection", rc == -1 && p.o.mode == N48OCC_DISABLED, "rc %d mode %d", rc, p.o.mode); }
    { int e; (void)n48occ_set(&p.o, N48OCC_DISABLED, 4096, &e); CHECK("Disabled takes any offset", p.o.mode == N48OCC_DISABLED, "mode %d", p.o.mode); }
    p_begin(&p, 4, 0); { int e; int rc = n48occ_set(&p.o, N48OCC_BOOLEAN, 0, &e); CHECK("a 4-byte buffer cannot hold an 8-byte result", rc == -1, "rc %d", rc); }
    p_begin(&p, 64, 0); p_set(&p, N48OCC_COUNTING, 3); p_draw(&p, 99); p_resolve(&p, b, 1);
    CHECK("a rejected selection writes nothing and counts nothing", b[1] == 77 && p.begun == 0 && p.o.nu == 0, "[8]=%llu begun %d nu %u", (unsigned long long)b[1], p.begun, p.o.nu);

    // ---- the fail-safe: "visible", never zero
    memset(b, 0, sizeof b);
    p_begin(&p, 64, 0); p_set(&p, N48OCC_BOOLEAN, 0); p_draw(&p, 0); p_set(&p, N48OCC_COUNTING, 8); p_draw(&p, 0); p_resolve(&p, b, 0);
    CHECK("results not trustworthy: Boolean -> 1", b[0] == 1, "[0]=%llu", (unsigned long long)b[0]);
    CHECK("results not trustworthy: Counting -> non-zero", b[1] != 0, "[8]=%llu", (unsigned long long)b[1]);
    memset(b, 0, sizeof b);
    p_begin(&p, 64, 0); p_set(&p, N48OCC_BOOLEAN, 0); n48occ_dead(&p.o); p_draw(&p, 0); p_resolve(&p, b, 1);
    CHECK("pool could not be created (dead): the selected offset is answered visible, no query begun", b[0] == 1 && p.begun == 0, "[0]=%llu begun %d", (unsigned long long)b[0], p.begun);
    memset(b, 0, sizeof b);
    p_begin(&p, 4096, 0); p_set(&p, N48OCC_BOOLEAN, 0);
    for (int i = 0; i < N48OCC_MAXQ + 5; i++) { p_split(&p); p_draw(&p, 0); }   // more intervals than slots, all hidden
    p_resolve(&p, b, 1);
    CHECK("more intervals than query slots: overflow, offset answered visible, never an out-of-range query", p.o.overflow && p.o.nq == N48OCC_MAXQ && b[0] == 1, "nq %u overflow %d [0]=%llu", p.o.nq, p.o.overflow, (unsigned long long)b[0]);
    memset(b, 0, sizeof b);
    p_begin(&p, 8ull * 1024, 0); for (int i = 0; i < N48OCC_MAXU + 3; i++) p_set(&p, N48OCC_BOOLEAN, 8ull * (uint64_t)i);
    CHECK("more distinct offsets than slots: overflow flagged", p.o.overflow && p.o.nu == N48OCC_MAXU, "nu %u overflow %d", p.o.nu, p.o.overflow);
    { uint64_t big[1024]; memset(big, 0, sizeof big); p_resolve(&p, big, 1);
      CHECK("overflow of the offset table: every offset that WAS tracked is answered visible", big[0] == 1 && big[N48OCC_MAXU - 1] == 1 && big[N48OCC_MAXU] == 0, "[0]=%llu [last]=%llu [untracked]=%llu", (unsigned long long)big[0], (unsigned long long)big[N48OCC_MAXU - 1], (unsigned long long)big[N48OCC_MAXU]); }
    // availability: one unavailable interval makes the whole pass untrustworthy
    { n48occ o; n48occ_init(&o, 64, 0); int e; (void)n48occ_set(&o, N48OCC_COUNTING, 0, &e); uint32_t q; int pr; (void)n48occ_draw(&o, &q, &pr); (void)n48occ_end(&o, &q); (void)n48occ_draw(&o, &q, &pr);
      uint64_t rawok[4] = { 10, 1, 20, 1 }, rawbad[4] = { 10, 1, 20, 0 }, res[2] = { 0, 0 };
      CHECK("availability: all intervals available -> trusted, results copied", n48occ_read(&o, rawok, res) == 1 && res[0] == 10 && res[1] == 20, "%llu %llu", (unsigned long long)res[0], (unsigned long long)res[1]);
      CHECK("availability: one interval unavailable -> NOT trusted", n48occ_read(&o, rawbad, res) == 0, "read %d", n48occ_read(&o, rawbad, res)); }
    // Boolean and Counting on one offset: Counting wins (S7: Apple's value there is undefined)
    memset(b, 0, sizeof b);
    p_begin(&p, 64, 0); p_set(&p, N48OCC_BOOLEAN, 0); p_draw(&p, 3); p_set(&p, N48OCC_COUNTING, 0); p_draw(&p, 4); p_resolve(&p, b, 1);
    CHECK("S7 mixed Boolean + Counting on one offset: the Counting sum", b[0] == 7, "[0]=%llu", (unsigned long long)b[0]);
    // precise only for Counting
    p_begin(&p, 64, 0); { uint32_t q; int pr = -1; p_set(&p, N48OCC_BOOLEAN, 0); (void)n48occ_draw(&p.o, &q, &pr); CHECK("Boolean uses the cheap query", pr == 0, "precise=%d", pr); }
    p_begin(&p, 64, 0); { uint32_t q; int pr = -1; p_set(&p, N48OCC_COUNTING, 0); (void)n48occ_draw(&p.o, &q, &pr); CHECK("Counting asks for a PRECISE query", pr == 1, "precise=%d", pr); }
    // repeated identical selection does not split the interval
    p_begin(&p, 64, 0); p_set(&p, N48OCC_COUNTING, 8); p_draw(&p, 1); p_set(&p, N48OCC_COUNTING, 8); p_draw(&p, 1); p_set(&p, N48OCC_COUNTING, 8); p_draw(&p, 1);
    CHECK("re-selecting the same mode and offset keeps one interval", p.begun == 1 && p.ended == 0, "begun %d ended %d", p.begun, p.ended);
    p_begin(&p, 64, 0); p_set(&p, N48OCC_COUNTING, 8); p_draw(&p, 1); p_set(&p, N48OCC_COUNTING, 16); p_draw(&p, 1); p_set(&p, N48OCC_BOOLEAN, 16); p_draw(&p, 1);
    CHECK("a change of offset or of mode ends the interval", p.begun == 3 && p.ended == 2, "begun %d ended %d", p.begun, p.ended);

    // ---- source pins on the glue (Navi48Device.m): ordering and reachability
    char path[512]; snprintf(path, sizeof path, "%s/Navi48Device.m", dir);
    FILE *f = fopen(path, "rb"); if (!f) { printf("FAIL: cannot open %s\n", path); return 2; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET); char *src = malloc((size_t)n + 1); if (fread(src, 1, (size_t)n, f) != (size_t)n) return 2; src[n] = 0; fclose(f);
    #define COUNT(needle) ({ int c_ = 0; for (const char *q_ = src; (q_ = strstr(q_, needle)); q_++) c_++; c_; })
    #define FIND(needle, from) ({ const char *r_ = strstr((from), (needle)); r_ ? r_ : NULL; })
    CHECK("pin: vkCmdEndRenderPass( is called from n48EndPass only", COUNT("vkCmdEndRenderPass(") == 1, "%d", COUNT("vkCmdEndRenderPass("));
    { const char *h = FIND("- (void)n48EndPass {", src); const char *e = h ? FIND("\n}\n", h) : NULL; const char *a = h ? FIND("[self n48OccEndQuery];", h) : NULL; const char *b2 = h ? FIND("vkCmdEndRenderPass(", h) : NULL;
      CHECK("pin: n48EndPass ends the open query BEFORE it ends the render pass", h && e && a && b2 && a < b2 && b2 < e, "helper present %d, order ok %d", h != NULL, (a && b2 && a < b2)); }
    CHECK("pin: every former pass-end site goes through n48EndPass (5 splits + endEncoding)", COUNT("[self n48EndPass]") == 6, "%d", COUNT("[self n48EndPass]"));
    { const char *pd = FIND("- (BOOL)n48PrepareDraw:", src); const char *bp = pd ? FIND("[self n48BeginPass];", pd) : NULL; const char *od = bp ? FIND("[self n48OccDraw];", bp) : NULL; const char *dr = FIND("- (void)drawPrimitives:(MTLPrimitiveType)t vertexStart:(NSUInteger)s vertexCount:(NSUInteger)c instanceCount:(NSUInteger)ic baseInstance", src);
      CHECK("pin: the query begins in n48PrepareDraw AFTER the render pass has begun, and before the draw is recorded", pd && bp && od && dr && pd < bp && bp < od && od < dr, "ordered %d", (pd && bp && od && dr && pd < bp && bp < od && od < dr)); }
    { const char *pd = FIND("- (void)n48PoolDone {", src); const char *e = pd ? FIND("\n", pd) : NULL; int ok = pd && e; if (ok) { const char *r = FIND("n48OccResolve", pd); const char *c = FIND("n48_fence_close", pd); ok = r && c && r < e && r < c; }
      CHECK("pin: n48PoolDone resolves the visibility buffers (before it closes the fence serial)", ok, "ok %d", ok); }
    { const char *a = FIND("if (r == VK_SUCCESS || r == VK_ERROR_DEVICE_LOST) [keep n48PoolDone];", src); const char *b2 = a ? FIND("[self commandBufferDidComplete:keep startTime:t1 completionTime:n48_now() error:werr];\n            N48LOGR(\"commandBufferDidComplete sent\")", a) : NULL;
      CHECK("pin: the resolve (n48PoolDone) happens BEFORE commandBufferDidComplete on the normal completion path", a && b2 && a < b2, "ordered %d", (a && b2 && a < b2)); }
    CHECK("pin: the write happens through n48occ_final (the arithmetic of this header), once per used offset", COUNT("n48occ_final(") == 1, "%d", COUNT("n48occ_final("));
    CHECK("pin: the results are read with the availability bit and without VK_QUERY_RESULT_WAIT_BIT", COUNT("VK_QUERY_RESULT_WITH_AVAILABILITY_BIT") == 1 && COUNT("VK_QUERY_RESULT_WAIT_BIT") == 0, "avail %d wait %d", COUNT("VK_QUERY_RESULT_WITH_AVAILABILITY_BIT"), COUNT("VK_QUERY_RESULT_WAIT_BIT"));
    CHECK("pin: the query pool is reset at encoder creation (outside the pass), once", COUNT("vkCmdResetQueryPool([_cb vk]") == 1, "%d", COUNT("vkCmdResetQueryPool([_cb vk]"));
    CHECK("pin: the PRECISE flag is requested only from the Counting decision", COUNT("VK_QUERY_CONTROL_PRECISE_BIT") == 1 && COUNT("(precise && N48R.occPrecise)") == 1, "%d", COUNT("VK_QUERY_CONTROL_PRECISE_BIT"));
    CHECK("pin: the dead-pool case marks the pass for the fail-safe", COUNT("if (*qp == VK_NULL_HANDLE) n48occ_dead(o);") == 1, "%d", COUNT("if (*qp == VK_NULL_HANDLE) n48occ_dead(o);"));
    CHECK("pin: a query is begun in exactly one place (n48OccDraw), with the interval from n48occ_draw", COUNT("vkCmdBeginQuery([_cb vk], _qp, _qbase + q") == 1 && COUNT("if (_occ && n48occ_draw(_occ, &q, &precise))") == 1, "%d", COUNT("vkCmdBeginQuery("));
    CHECK("pin: a query is ended in exactly two places: a mode / offset change and the pass end", COUNT("vkCmdEndQuery([_cb vk], _qp, _qbase + ") == 2 && COUNT("if (endq) vkCmdEndQuery(") == 1 && COUNT("if (_occ && n48occ_end(_occ, &q)) vkCmdEndQuery(") == 1, "%d", COUNT("vkCmdEndQuery("));
    CHECK("pin: the visibility result TYPE (Accumulate) is read from the pass descriptor", COUNT("acc = (long)[(id)d visibilityResultType] == 1;") == 1 && COUNT("[d respondsToSelector:@selector(visibilityResultType)]") == 1, "%d", COUNT("visibilityResultType"));
    CHECK("pin: the query indices are reserved and reset in the encoder's init, after the bookkeeping exists", ({ const char *i0 = FIND("- (void)n48OccInit:", src); const char *nw = i0 ? FIND("[_cb n48OccNew:", i0) : NULL; const char *rs = nw ? FIND("vkCmdResetQueryPool([_cb vk], _qp, _qbase, N48OCC_MAXQ)", nw) : NULL; const char *ie = i0 ? FIND("\n}\n", i0) : NULL; i0 && nw && rs && ie && nw < rs && rs < ie; }), "ordered");
    CHECK("pin: the encoder's init calls n48OccInit last (outside any render pass)", ({ const char *ri = FIND("- (instancetype)initWithCommandBuffer:(id)cb descriptor:(MTLRenderPassDescriptor *)d {", src); const char *oi = ri ? FIND("[self n48OccInit:d];", ri) : NULL; const char *rt = oi ? FIND("return self;", oi) : NULL; const char *nx = ri ? FIND("\n}\n", ri) : NULL; ri && oi && rt && nx && rt < nx; }), "ordered");
    CHECK("pin: the visibility buffer is retained for the command buffer's life (the resolve writes into it after the GPU is done)", COUNT("[_ojb addObject:vb]; [self n48Retain:vb];") == 1, "%d", COUNT("[_ojb addObject:vb]"));
    CHECK("pin: an offset is written only when the buffer has a CPU mapping and through the bookkeeping's own offset", COUNT("uint64_t *slot = (uint64_t *)(void *)(base + o->u[u].off);") == 1 && COUNT("for (uint32_t u = 0; u < o->nu && base; u++)") == 1, "%d", COUNT("o->nu && base"));
    CHECK("pin: a command buffer whose fence is not VK_SUCCESS answers visible (the fence is asked, not waited for)", COUNT("ran = vkGetFenceStatus(N48R.dev, _fence) == VK_SUCCESS;") == 1 && COUNT("BOOL ok = ran && _oj[j].qp != VK_NULL_HANDLE;") == 1, "%d", COUNT("vkGetFenceStatus"));
    CHECK("pin: a visibility buffer that is not a CPU-mapped N48Buffer is never dereferenced", COUNT("if (![vb isKindOfClass:[N48Buffer class]] || ![(N48Buffer *)vb contents]) {") == 1, "%d", COUNT("![vb isKindOfClass:[N48Buffer class]]"));
    CHECK("pin: the raw query results go through n48occ_read (availability checked in the tested arithmetic)", COUNT("if (ok && !n48occ_read(o, raw, res)) ok = NO;") == 1, "%d", COUNT("n48occ_read("));
    CHECK("pin: occlusionQueryPrecise is enabled on the device only when the physical device has it", COUNT(".occlusionQueryPrecise = pf.occlusionQueryPrecise") == 1, "%d", COUNT(".occlusionQueryPrecise = pf.occlusionQueryPrecise"));
    printf("%s (%d failure(s))\n", fails ? "FAILED" : "all passed", fails);
    return fails ? 1 : 0;
}
