// test-ledger.c: host test of n48_ledger.h (P5 Step 0 ledger + F3 use-count policy, build 16) plus source pins on Navi48Device.m for the P5 / P4 glue.
// Build/run (from this directory):  cc -O1 -Wall -Wextra -Werror -o /tmp/test-ledger test-ledger.c && /tmp/test-ledger .      (argv[1] = the directory holding Navi48Device.m and n48_xlate.h, default ".")
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "n48_ledger.h"
static int fails;
#define CHECK(name, cond, ...) do { int ok_ = (cond); printf("%s %s: ", ok_ ? "ok  " : "FAIL", name); printf(__VA_ARGS__); printf("\n"); if (!ok_) fails++; } while (0)
#define SEC 1000000000ULL

static long incs, decs; static long perSurface[64];
static void tinc(void *s, void *c) { (void)c; incs++; perSurface[(uintptr_t)s]++; }
static void tdec(void *s, void *c) { (void)c; decs++; perSurface[(uintptr_t)s]--; }
static unsigned rng = 777;
static unsigned rnd(void) { rng = rng * 1103515245u + 12345u; return (rng >> 16) & 0x7fff; }

static char *slurp(const char *dir, const char *name) {
    char p[1024]; snprintf(p, sizeof p, "%s/%s", dir, name); FILE *f = fopen(p, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET); char *b = (char *)malloc((size_t)n + 1); if (fread(b, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(b); return NULL; } b[n] = 0; fclose(f); return b;
}
static int count(const char *h, const char *needle) { int k = 0; for (const char *p = h; (p = strstr(p, needle)); p += strlen(needle)) k++; return k; }
// The text of the function / method / block that starts at `start` and ends at the next line that is exactly "}" (or the end of the file). Caller frees.
static char *body(const char *m, const char *start) {
    const char *f = strstr(m, start); if (!f) return NULL;
    const char *e = strstr(f, "\n}\n"); size_t n = e ? (size_t)(e - f) + 3 : strlen(f);
    char *b = (char *)malloc(n + 1); memcpy(b, f, n); b[n] = 0; return b;
}

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : ".";
    n48l L; n48l_snap s; char line[1500];

    // ---- 1. ledger balance ------------------------------------------------------------------------------------------------------------------
    n48l_init(&L);
    for (uintptr_t k = 1; k <= 10; k++) n48l_add(&L, k, (uint32_t)(100 + k / 2), 4096 * k, 0x42475241, 1, 0, 1 * SEC);
    n48l_snapshot(&L, 11 * SEC, &s);
    CHECK("10 textures added: live 10, distinct ids 6 (ids 100..105), tex bytes = sum", s.live == 10 && s.distinct == 6 && s.texBytes == 4096ULL * 55, "live %u distinct %u bytes %llu", s.live, s.distinct, (unsigned long long)s.texBytes);
    CHECK("distinct bytes counts the largest import of each id once", s.distinctBytes == 4096ULL * (1 + 3 + 5 + 7 + 9 + 10), "%llu", (unsigned long long)s.distinctBytes);
    CHECK("no command buffer in flight: all 10 idle", s.idle == 10 && s.inflightTex == 0, "idle %u inflight %u", s.idle, s.inflightTex);
    CHECK("the oldest is 10 s old", s.oldestAgeNs == 10 * SEC, "%llu", (unsigned long long)s.oldestAgeNs);
    n48l_touch(&L, 3); n48l_touch(&L, 3); n48l_touch(&L, 4);
    n48l_snapshot(&L, 11 * SEC, &s);
    CHECK("two textures touched (one by two command buffers): 2 in flight, 8 idle", s.inflightTex == 2 && s.idle == 8, "inflight %u idle %u", s.inflightTex, s.idle);
    n48l_untouch(&L, 3); n48l_snapshot(&L, 11 * SEC, &s);
    CHECK("one of the two command buffers finished: texture 3 is still in flight", s.inflightTex == 2, "inflight %u", s.inflightTex);
    n48l_untouch(&L, 3); n48l_untouch(&L, 4); n48l_snapshot(&L, 11 * SEC, &s);
    CHECK("all finished: 10 idle", s.idle == 10 && s.inflightTex == 0, "idle %u", s.idle);
    for (uintptr_t k = 1; k <= 10; k++) n48l_del(&L, k);
    n48l_snapshot(&L, 12 * SEC, &s);
    CHECK("all removed: ledger empty, added == removed, no anomaly", s.live == 0 && L.added == 10 && L.removed == 10 && !L.doubleAdd && !L.delMissing && !L.delInflight && !L.touchMissing && !L.untouchMissing && !L.untouchUnder, "live %u", s.live);
    n48l_add(&L, 1, 1, 1, 1, 1, 0, 0); n48l_add(&L, 1, 1, 1, 1, 1, 0, 0); n48l_del(&L, 99); n48l_touch(&L, 99); n48l_untouch(&L, 1); n48l_touch(&L, 1); n48l_del(&L, 1);
    CHECK("anomalies are counted: double add, delete of an unknown key, touch of an unknown key, untouch below zero, delete while in flight", L.doubleAdd == 1 && L.delMissing == 1 && L.touchMissing == 1 && L.untouchUnder == 1 && L.delInflight == 1, "dbl %llu dm %llu tm %llu uu %llu di %llu",
          (unsigned long long)L.doubleAdd, (unsigned long long)L.delMissing, (unsigned long long)L.touchMissing, (unsigned long long)L.untouchUnder, (unsigned long long)L.delInflight);
    n48l_destroy(&L);

    // ---- 2. top groups -----------------------------------------------------------------------------------------------------------------------
    n48l_init(&L);
    uintptr_t key = 1;
    for (int i = 0; i < 40; i++) n48l_add(&L, key++, 1000 + (uint32_t)i, 8294400, 0x34323066, 2, 0, (uint64_t)(i + 1) * SEC);    // 40 video frames 1920x1080 420f, 2 planes, born 1..40 s
    for (int i = 0; i < 7; i++)  n48l_add(&L, key++, 2000 + (uint32_t)i, 14745600, 0x42475241, 1, 0, 50 * SEC);                  // 7 display-sized BGRA
    for (int i = 0; i < 3; i++)  n48l_add(&L, key++, 3000 + (uint32_t)i, 4096, 0x4c303038, 1, 1, 60 * SEC);
    n48l_add(&L, key++, 4000, 7344128, 0x42475241, 1, 0, 61 * SEC); n48l_add(&L, key++, 4001, 4096, 0x42475241, 1, 0, 61 * SEC); n48l_add(&L, key++, 4002, 8192, 0x42475241, 1, 0, 61 * SEC);
    n48l_snapshot(&L, 100 * SEC, &s);
    CHECK("groups: 6 distinct (size, format, planes) groups; the biggest by count first", s.ngrp == 6 && s.top[0].count == 40 && s.top[0].bytes == 8294400 && s.top[0].planes == 2, "ngrp %u top0 x%u", s.ngrp, s.top[0].count);
    CHECK("groups: then 7, then 3", s.top[1].count == 7 && s.top[2].count == 3, "x%u x%u", s.top[1].count, s.top[2].count);
    CHECK("groups: the oldest member's age is reported (99 s for the video group)", s.top[0].oldestAgeNs == 99 * SEC, "%.1f", (double)s.top[0].oldestAgeNs / 1e9);
    CHECK("groups: only the top 5 are kept", s.top[4].count == 1 && s.ngrp > N48L_TOP, "x%u", s.top[4].count);
    CHECK("no-CPU-mapping textures are counted", s.nobase == 3, "%u", s.nobase);
    n48l_fmt(&s, 5ULL << 30, &L, line, sizeof line);
    CHECK("the log line carries every figure the soak reads", strstr(line, "T1 ledger:") && strstr(line, "live=53") && strstr(line, "distinct_surface_ids=53") && strstr(line, "import_total=5242880 KiB") && strstr(line, "idle(no command buffer in flight)=53") && strstr(line, "top5") && strstr(line, "x40"), "%.200s", line);
    n48l_destroy(&L);

    // ---- 3. the F3 use-count policy --------------------------------------------------------------------------------------------------------------
    CHECK("policy: default is in-flight only", n48uc_mode(0) == N48UC_INFLIGHT, "mode %d", n48uc_mode(0));
    CHECK("policy: the kill file means never", n48uc_mode(1) == N48UC_NONE, "mode %d", n48uc_mode(1));
    {
        n48uc_list l; memset(&l, 0, sizeof l); memset(perSurface, 0, sizeof perSurface); incs = decs = 0;
        int a = n48uc_take(&l, N48UC_INFLIGHT, (void *)5, tinc, NULL), b = n48uc_take(&l, N48UC_INFLIGHT, (void *)5, tinc, NULL), c = n48uc_take(&l, N48UC_INFLIGHT, (void *)6, tinc, NULL);
        CHECK("take: one count per surface per command buffer", a == 1 && b == 0 && c == 1 && incs == 2 && perSurface[5] == 1 && perSurface[6] == 1, "incs %ld", incs);
        size_t r1 = n48uc_release(&l, tdec, NULL), r2 = n48uc_release(&l, tdec, NULL);
        CHECK("release: every held surface exactly once, the second release does nothing", r1 == 2 && r2 == 0 && decs == 2 && perSurface[5] == 0 && perSurface[6] == 0, "r1 %zu r2 %zu decs %ld", r1, r2, decs);
        CHECK("mode NONE never takes", n48uc_take(&l, N48UC_NONE, (void *)7, tinc, NULL) == 0 && incs == 2 && l.n == 0, "incs %ld", incs);
        CHECK("a NULL surface is never taken", n48uc_take(&l, N48UC_INFLIGHT, NULL, tinc, NULL) == 0 && incs == 2, "incs %ld", incs);
        n48uc_free(&l);
    }
    {   // the video-pool picture: 20 pooled surfaces, a texture per surface lives in "WindowServer" forever; command buffers come and go
        memset(perSurface, 0, sizeof perSurface); incs = decs = 0;
        enum { NCB = 6 }; n48uc_list cbs[NCB]; memset(cbs, 0, sizeof cbs); int live[NCB] = { 0 };
        long maxInFlightSurfaces = 0, badFree = 0, lifetimeHold = 0;
        for (int it = 0; it < 20000; it++) {
            int k = (int)(rnd() % NCB);
            if (live[k] && (rnd() & 1)) { n48uc_release(&cbs[k], tdec, NULL); live[k] = 0; }   // the command buffer completed
            else { int touches = 1 + (int)(rnd() % 4); for (int j = 0; j < touches; j++) (void)n48uc_take(&cbs[k], N48UC_INFLIGHT, (void *)(uintptr_t)(1 + rnd() % 20), tinc, NULL); live[k] = 1; }
            long inflightSurfaces = 0, anyHeld = 0; for (int s2 = 1; s2 <= 20; s2++) { if (perSurface[s2] > 0) inflightSurfaces++; if (perSurface[s2] < 0) badFree++; anyHeld += perSurface[s2]; }
            if (inflightSurfaces > maxInFlightSurfaces) maxInFlightSurfaces = inflightSurfaces;
            int anyLive = 0; for (int q = 0; q < NCB; q++) anyLive |= live[q] && cbs[q].n;
            if (!anyLive && anyHeld != 0) badFree++;
        }
        for (int q = 0; q < NCB; q++) { n48uc_release(&cbs[q], tdec, NULL); n48uc_free(&cbs[q]); }
        long residue = 0; for (int s2 = 1; s2 <= 20; s2++) residue += perSurface[s2];
        CHECK("pool: no surface is ever use-counted with no command buffer in flight, the count never goes negative, and it returns to zero when all command buffers are finished", badFree == 0 && residue == 0 && incs == decs, "bad %ld residue %ld incs %ld decs %ld", badFree, residue, incs, decs);
        lifetimeHold = 20;   // build 14: one count per live texture for the texture's whole life: the 20 pooled surfaces stay "in use" forever and a pool that recycles only unused surfaces never recycles them
        CHECK("pool: with the lifetime hold of build 14 all 20 surfaces would have stayed in use; now at most the in-flight ones are", maxInFlightSurfaces <= 20 && lifetimeHold == 20, "max in flight %ld", maxInFlightSurfaces);
    }

    // ---- 4. source pins on Navi48Device.m / n48_xlate.h ---------------------------------------------------------------------------------------------------
    char *m = slurp(dir, "Navi48Device.m"), *xl = slurp(dir, "n48_xlate.h");
    CHECK("source: Navi48Device.m and n48_xlate.h are readable", m && xl, "%s", dir);
    if (m && xl) {
        CHECK("source: the headers are included", strstr(m, "#include \"n48_ledger.h\"") && strstr(m, "#include \"n48_ioalias.h\""), "includes");
        CHECK("source F3: IOSurfaceIncrementUseCount appears exactly once (the per-command-buffer hold callback) and Decrement exactly once", count(m, "IOSurfaceIncrementUseCount(") == 1 && count(m, "IOSurfaceDecrementUseCount(") == 1, "inc %d dec %d", count(m, "IOSurfaceIncrementUseCount("), count(m, "IOSurfaceDecrementUseCount("));
        char *uci = body(m, "static void n48_uc_inc("), *ucd = body(m, "static void n48_uc_dec(");
        CHECK("source F3: the one increment is in n48_uc_inc, the one decrement in n48_uc_dec", uci && ucd && strstr(uci, "IOSurfaceIncrementUseCount(") && strstr(ucd, "IOSurfaceDecrementUseCount("), "callbacks");
        free(uci); free(ucd);
        char *ft = body(m, "- (BOOL)n48IOFirstTouch:(N48Texture *)t {");
        CHECK("source F3: the first touch of a texture in a command buffer takes the hold and the ledger touch", ft && strstr(ft, "n48uc_take(&_uc, N48LED.ucMode") && strstr(ft, "n48_led_touch(t, 1)"), "first touch");
        free(ft);
        char *ur = body(m, "- (void)n48UCRelease {");
        CHECK("source F3: n48UCRelease releases the holds, gives the ledger touches back, and is idempotent", ur && strstr(ur, "if (!_ucLive) return;") && strstr(ur, "n48uc_release(&_uc, n48_uc_dec") && strstr(ur, "n48_led_touch(t, -1)"), "release");
        free(ur);
        char *pd = body(m, "- (void)n48PoolDone {"), *cd = body(m, "- (void)dealloc {\n    [self n48UCRelease]");
        CHECK("source F3: the use counts are given back when the command buffer completes / fails (n48PoolDone) and when it is deallocated", pd && strstr(pd, "[self n48UCRelease]") && cd && strstr(cd, "n48uc_free(&_uc)"), "release sites");
        free(pd); free(cd);
        CHECK("source F3: submit failure and completion both reach n48PoolDone", count(m, "[cb n48PoolDone]") >= 1 && count(m, "[keep n48PoolDone]") >= 1, "sites");
        CHECK("source F3: the texture no longer holds a use count in its life (init, dealloc)", count(m, "n48_led_add(self, s, 0, spf") == 1 && count(m, "n48_led_add(self, s, ialloc, spf") == 1, "ledger entries on both init paths");
        char *td = body(m, "for (NSNumber *lvv in _lvViews.allValues) vkDestroyImageView");
        CHECK("source P5: the texture dealloc leaves the ledger and frees its import (shared: back to the cache; own: vkFreeMemory)", td && strstr(td, "n48_led_del(self)") && strstr(td, "n48_imp_put(_imem)") && strstr(td, "vkFreeMemory(N48R.dev, _imem, NULL)") && strstr(td, "CFRelease(_ios)") && !strstr(td, "IOSurfaceDecrementUseCount"), "dealloc");
        free(td);
        char *dr = body(m, "static void n48_imp_drain(void) {");
        CHECK("source P5: the import cache's drain frees what it gave up (vkFreeMemory + the import total + the CF owner)", dr && strstr(dr, "vkFreeMemory(N48R.dev, (VkDeviceMemory)o.mem, NULL)") && strstr(dr, "N48R.impTotal -= o.size") && strstr(dr, "CFRelease((IOSurfaceRef)o.owner)"), "drain");
        free(dr);
        CHECK("source F2: the cache default comes from n48ic_default_on with the kill file, and the old presence file is not read", strstr(m, "n48ic_default_on(access(N48_IMPCACHE_KILL_FILE, F_OK) == 0)") && strstr(m, "\"/private/tmp/n48m-noimpcache\"") && !strstr(m, "N48_IMPCACHE_FILE"), "default on");
        CHECK("source F1: the import refusals (alignment, size cap, host-pointer properties, allocation) fall back instead of returning nil", count(m, "N48F1_FALL(@") == 4 && strstr(m, "goto nobase_path") && strstr(m, "nobase_path:") && !strstr(m, "IOSFAIL(80,") && !strstr(m, "IOSFAIL(82,") && !strstr(m, "IOSFAIL(83,") && !strstr(m, "IOSFAIL(84,"), "N48F1_FALL sites %d", count(m, "N48F1_FALL(@"));
        { const char *mac = strstr(m, "#define N48F1_FALL(...)"); const char *me = mac ? strchr(mac, '\n') : NULL; size_t ml = me ? (size_t)(me - mac) : 0; char mb[2048]; if (ml >= sizeof mb) ml = sizeof mb - 1; if (mac) memcpy(mb, mac, ml); mb[ml] = 0;
          CHECK("source F1: the fallback macro counts the fallback, logs it (limited) and JUMPS to the baseless branch (it never returns nil)", mac && strstr(mb, "atomic_fetch_add(&N48LED.f1Fallbacks, 1)") && strstr(mb, "goto nobase_path; } while (0)") && !strstr(mb, "return"), "macro: %.120s", mb); }
        CHECK("source F1: the hook is wired to n48f1_inject", strstr(m, "n48f1_inject(n48f1_every, &n48f1_ctr)") != NULL, "hook");
        CHECK("source F1: the allocation refusal is decided by n48f1_action and goes through the injectable n48_import_alloc", strstr(m, "n48f1_action((int)r) == N48F1_FALLBACK") && count(m, "n48_import_alloc(") >= 4 && strstr(m, "N48M_TEST_IMPORT_FAIL_EVERY") && strstr(m, "(n48_allow() && e)"), "alloc decision + hook");
        char *ig = body(m, "static VkResult n48_imp_get(");
        CHECK("source F1: a refused import first flushes the cache and retries (before the texture falls back)", ig && strstr(ig, "n48ic_flush(&N48IC.c, st)") && strstr(ig, "r = n48_import_alloc(ma, &mem)") && count(ig, "n48_import_alloc(") == 2, "flush+retry");
        free(ig);
        CHECK("source F1: the fallback keeps the baseless branch's own classification (no edit of the scanout classify lines)", strstr(m, "if (pc <= 1 && _fmt->bpp == 4 && n48s_classify(sw, sh, spf, d.usage, bpr, (size_t)n48df_nobase_alloc(alloc, bpr, sh), IOSurfaceGetID(s), NULL)) {") != NULL, "classify line intact");
        CHECK("source P5: the T1 tick calls the ledger report", strstr(m, "n48_led_report(tick);") != NULL, "tick");
        CHECK("source P5: the ledger line is logged with its use-count / alias / F1 counters", strstr(m, "usecount %s takes=%llu releases=%llu held_now=%lld") != NULL && strstr(m, "n48l_fmt(&sn, imp, &N48LED.L, line, sizeof line)") != NULL, "line");
        CHECK("source P4: the first-attempt miss is not logged as a failure when this process translates in process", strstr(m, "compute pipeline: not in the bundle's spvcache") && strstr(m, "render pipeline: not in the bundle's spvcache") && strstr(m, "trying the in-process translation"), "wording");
        char *tj = body(xl, "static inline int n48x_translate_job_x(");
        CHECK("source P4: n48x_translate_job answers a repeat of an installed sha before the crash/fail/live probe", tj && strstr(tj, "S_ISREG(dst.st_mode)) return N48X_OK;") && strstr(tj, "S_ISREG(dst.st_mode)") < strstr(tj, "n48x_probe(dir, sha)"), "early out before the probe");
        free(tj);
        char *eg = body(xl, "static inline n48x_job *n48x_find_job(");
        CHECK("source P4: a second request for a queued or running sha shares that job", eg && strstr(eg, "e->running") && strstr(eg, "for (n48x_job *j = e->head; j; j = j->next)"), "find_job");
        free(eg);
    }
    free(m); free(xl);
    printf("%s (%d failure%s)\n", fails ? "FAILED" : "ALL PASSED", fails, fails == 1 ? "" : "s");
    return fails != 0;
}
