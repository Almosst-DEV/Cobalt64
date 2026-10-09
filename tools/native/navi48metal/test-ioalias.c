// test-ioalias.c: host test of n48_ioalias.h (P1, build 16) plus source pins on Navi48Device.m for the glue.
// Build/run (from this directory):  cc -O1 -Wall -Wextra -Werror -o /tmp/test-ioalias test-ioalias.c && /tmp/test-ioalias .      (argv[1] = the directory holding Navi48Device.m, default ".")
//
// The core is a DIFFERENTIAL test: random sequences of touches (object, path a/b, needs contents, writes, range) are replayed twice, once against an ideal single-truth model and once through a tiny executor that does
// exactly what Navi48Device.m's n48_ios_touch does with the decisions (flush = copy the writer's image into the pages and drop it from the write-back list; upload = copy the pages into the image; end of the
// command buffer = write back every object still in the list, in list order). Every read must see the newest value, and the pages at the end must hold it.
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "n48_ioalias.h"
static int fails;
#define CHECK(name, cond, ...) do { int ok_ = (cond); printf("%s %s: ", ok_ ? "ok  " : "FAIL", name); printf(__VA_ARGS__); printf("\n"); if (!ok_) fails++; } while (0)
#define NR 3          // ranges: (sid 7, off 0), (sid 7, off 65536) = the second plane of the same surface, (sid 9, off 0)
#define NO_ 5         // objects
static const uint32_t RSID[NR] = { 7, 7, 9 };
static const uint64_t ROFF[NR] = { 0, 65536, 0 };

typedef struct { int H[NR]; int img[NO_][NR]; int iow[NR * NO_ + 1]; int niow; int ideal[NR]; int seq; int flushes, uploads; int readBad, wbCount; } world;   // iow: object index * NR + range
static int isBof(int o) { return o % 2 == 0; }   // objects 0,2,4 are path (b); 1,3 are path (a)
static void iow_remove(world *w, int key) { for (int i = 0; i < w->niow; i++) if (w->iow[i] == key) { memmove(&w->iow[i], &w->iow[i + 1], (size_t)(w->niow - i - 1) * sizeof(int)); w->niow--; return; } }
static void iow_add(world *w, int key) { for (int i = 0; i < w->niow; i++) if (w->iow[i] == key) return; w->iow[w->niow++] = key; }
static void step(world *w, n48ia *a, int obj, int rg, int need, int write, int actOn) {
    int isB = isBof(obj);
    n48ia_act d = n48ia_touch(a, RSID[rg], ROFF[rg], (uintptr_t)(obj + 1), isB, need, write);
    if (actOn) {
        if (d.flush) { int x = (int)d.flush - 1; w->H[rg] = w->img[x][rg]; iow_remove(w, x * NR + rg); w->flushes++; }
        if (d.upload && isB) { w->img[obj][rg] = w->H[rg]; w->uploads++; }
    } else {   // build 14: upload exactly at the first touch of the object in the command buffer (needs contents)
        static int touched[NO_][NR]; static world *last; if (last != w) { memset(touched, 0, sizeof touched); last = w; }
        if (!touched[obj][rg]) { touched[obj][rg] = 1; if (need && isB) w->img[obj][rg] = w->H[rg]; }
    }
    if (need && !write) {   // a read: must see the newest value
        int seen = isB ? w->img[obj][rg] : w->H[rg];
        if (seen != w->ideal[rg]) w->readBad++;
    }
    if (write) {
        if (need) { int seen = isB ? w->img[obj][rg] : w->H[rg]; if (seen != w->ideal[rg]) w->readBad++; }   // a read-modify-write needs the newest first
        int v = ++w->seq; w->ideal[rg] = v;
        if (isB) { w->img[obj][rg] = v; iow_add(w, obj * NR + rg); } else w->H[rg] = v;
    }
}
static void finish(world *w) { for (int i = 0; i < w->niow; i++) { int o = w->iow[i] / NR, rg = w->iow[i] % NR; w->H[rg] = w->img[o][rg]; w->wbCount++; } }
static unsigned rng = 12345;
static unsigned rnd(void) { rng = rng * 1103515245u + 12345u; return (rng >> 16) & 0x7fff; }

static char *slurp(const char *dir, const char *name) {
    char p[1024]; snprintf(p, sizeof p, "%s/%s", dir, name); FILE *f = fopen(p, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET); char *b = (char *)malloc((size_t)n + 1); if (fread(b, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(b); return NULL; } b[n] = 0; fclose(f); return b;
}
static int count(const char *h, const char *needle) { int k = 0; for (const char *p = h; (p = strstr(p, needle)); p += strlen(needle)) k++; return k; }

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : ".";
    n48ia a; world w;

    // ---- 1. the gate -------------------------------------------------------------------------------------------------------------------------
    CHECK("gate: an application with no kill file acts", n48ia_enabled(0, 0) == 1, "enabled");
    CHECK("gate: WindowServer never acts", n48ia_enabled(1, 0) == 0, "disabled");
    CHECK("gate: the kill file turns it off in applications", n48ia_enabled(0, 1) == 0, "disabled");
    CHECK("gate: WindowServer with the kill file is off too", n48ia_enabled(1, 1) == 0, "disabled");

    // ---- 2. the Preview scenario of the design: A (b) written by a blit, B (b) re-wrapped to read, same surface, one command buffer -----------
    n48ia_init(&a); memset(&w, 0, sizeof w);
    step(&w, &a, 0, 0, 0, 1, 1);    // A: full overwrite (blit), image = v1, pages stale
    CHECK("scenario: the writer is dirty and nothing else happened yet", w.H[0] == 0 && w.niow == 1, "H %d niow %d", w.H[0], w.niow);
    n48ia_act d = n48ia_touch(&a, 7, 0, 3, 1, 1, 0);   // B (another b object) reads
    CHECK("scenario: the reader B makes A copy back first and uploads", d.flush == 1 && d.upload == 1 && d.reupload == 0, "flush %zu upload %d reupload %d", (size_t)d.flush, d.upload, d.reupload);
    n48ia_destroy(&a);

    // ---- 3. targeted cases --------------------------------------------------------------------------------------------------------------------
    n48ia_init(&a); memset(&w, 0, sizeof w);
    step(&w, &a, 0, 0, 0, 1, 1); step(&w, &a, 2, 0, 1, 0, 1);
    CHECK("b writes, another b reads: the reader sees it, the writer was flushed", w.readBad == 0 && w.flushes == 1 && w.uploads == 1 && w.niow == 0, "bad %d flush %d up %d niow %d", w.readBad, w.flushes, w.uploads, w.niow);
    n48ia_destroy(&a);
    n48ia_init(&a); memset(&w, 0, sizeof w);
    step(&w, &a, 0, 0, 0, 1, 1); step(&w, &a, 1, 0, 0, 1, 1); finish(&w);
    CHECK("CLOBBER: b writes, then a linear sibling writes: the pages keep the sibling's newer bytes", w.H[0] == w.ideal[0] && w.ideal[0] == 2 && w.wbCount == 0, "H %d ideal %d writebacks %d", w.H[0], w.ideal[0], w.wbCount);
    n48ia_destroy(&a);
    n48ia_init(&a); memset(&w, 0, sizeof w);
    step(&w, &a, 2, 0, 1, 0, 1);                    // C (b) reads the pages (first touch: upload)
    step(&w, &a, 0, 0, 0, 1, 1);                    // A (b) writes
    step(&w, &a, 2, 0, 1, 0, 1);                    // C reads again: ITS IMAGE IS STALE -> flush A and re-upload C even though C was touched before
    CHECK("forced re-upload: a b object touched before is refreshed after another object wrote", w.readBad == 0 && w.uploads == 2 && w.flushes == 1, "bad %d up %d flush %d", w.readBad, w.uploads, w.flushes);
    n48ia_destroy(&a);
    n48ia_init(&a); memset(&w, 0, sizeof w);
    step(&w, &a, 0, 0, 0, 1, 1); step(&w, &a, 2, 1, 1, 0, 1);   // write plane 0 with A, read plane 1 with C
    CHECK("two planes of one surface are independent: no flush, A stays dirty", w.flushes == 0 && w.niow == 1, "flush %d niow %d", w.flushes, w.niow);
    step(&w, &a, 2, 2, 1, 0, 1);                                  // another surface entirely
    CHECK("another surface is independent too", w.flushes == 0, "flush %d", w.flushes);
    n48ia_destroy(&a);
    n48ia_init(&a); memset(&w, 0, sizeof w);
    step(&w, &a, 0, 0, 0, 1, 1); step(&w, &a, 0, 0, 1, 0, 1);    // the writer reads its own result
    CHECK("the writer touching its own range does not flush itself", w.flushes == 0 && w.readBad == 0, "flush %d bad %d", w.flushes, w.readBad);
    n48ia_destroy(&a);
    n48ia_init(&a); memset(&w, 0, sizeof w);
    step(&w, &a, 0, 0, 0, 1, 1); step(&w, &a, 1, 0, 1, 0, 1);    // linear reader after a b writer
    CHECK("a linear reader after a b writer gets the written bytes", w.readBad == 0 && w.flushes == 1, "bad %d flush %d", w.readBad, w.flushes);
    n48ia_destroy(&a);
    n48ia_init(&a); memset(&w, 0, sizeof w);
    step(&w, &a, 1, 0, 1, 1, 1); step(&w, &a, 0, 0, 1, 0, 1);    // linear read-modify-write, then a b reader
    CHECK("linear writer then b reader: no flush needed, the reader uploads the new bytes", w.readBad == 0 && w.flushes == 0 && w.uploads >= 1, "bad %d flush %d up %d", w.readBad, w.flushes, w.uploads);
    n48ia_destroy(&a);
    {   // the old behaviour (what WindowServer keeps) really fails the Preview scenario: the harness can see the bug
        n48ia_init(&a); memset(&w, 0, sizeof w);
        step(&w, &a, 0, 0, 0, 1, 0); step(&w, &a, 2, 0, 1, 0, 0);
        CHECK("harness sanity: the build-14 behaviour reads stale pages in the Preview scenario", w.readBad == 1, "bad reads %d", w.readBad);
        n48ia_destroy(&a);
    }

    // ---- 4. differential: 20000 random command buffers --------------------------------------------------------------------------------------------
    {
        int badBufs = 0, totalFlush = 0, totalUp = 0;
        for (int it = 0; it < 20000; it++) {
            n48ia_init(&a); memset(&w, 0, sizeof w);
            int nops = 2 + (int)(rnd() % 14);
            for (int k = 0; k < nops; k++) {
                int obj = (int)(rnd() % NO_), rg = (int)(rnd() % NR), need = (int)(rnd() & 1), write = (int)(rnd() % 3 == 0);
                step(&w, &a, obj, rg, need, write, 1);
            }
            finish(&w);
            int endBad = 0; for (int rg = 0; rg < NR; rg++) if (w.H[rg] != w.ideal[rg]) endBad++;
            if (w.readBad || endBad) badBufs++;
            totalFlush += w.flushes; totalUp += w.uploads;
            uintptr_t dirty[16]; size_t nd = n48ia_dirty(&a, dirty, 16);
            if ((int)nd != w.niow) badBufs++;   // the machine's idea of "still dirty" is exactly the executor's write-back list
            n48ia_destroy(&a);
        }
        CHECK("differential: 20000 random command buffers read the newest value everywhere and end with the newest pages", badBufs == 0, "%d bad buffers (flushes %d uploads %d)", badBufs, totalFlush, totalUp);
        CHECK("differential: the random run exercised flushes and re-uploads", totalFlush > 5000 && totalUp > 5000, "flushes %d uploads %d", totalFlush, totalUp);
    }

    // ---- 5. glue pins on Navi48Device.m ---------------------------------------------------------------------------------------------------------------
    char *m = slurp(dir, "Navi48Device.m");
    CHECK("source: Navi48Device.m is readable", m != NULL, "%s/Navi48Device.m", dir);
    if (m) {
        CHECK("source: the P1 header is included", strstr(m, "#include \"n48_ioalias.h\"") != NULL, "include");
        CHECK("source: the gate is n48ia_enabled(n48_is_ws(), <kill file>)", strstr(m, "n48ia_enabled(n48_is_ws() ? 1 : 0, ") != NULL && strstr(m, "/private/tmp/n48m-noioalias") != NULL, "gate + kill file");
        CHECK("source: both render-encoder sampling sites and the generic site go through n48_ios_touch", count(m, "n48_ios_touch(") >= 4, "%d call sites/definition", count(m, "n48_ios_touch("));
        CHECK("source: neither render-encoder site still uploads on the bare first-touch test", count(m, "if ([t n48IsIOS] && [_cb n48IOFirstTouch:t]) {") == 0 && count(m, "if ([t n48IsIOS] && [self->_cb n48IOFirstTouch:t]) {") == 0, "legacy first-touch upload blocks");
        const char *f = strstr(m, "static BOOL n48_ios_touch(");
        const char *fe = f ? strstr(f, "\n}\n") : NULL;
        CHECK("source: n48_ios_touch exists", f && fe, "found");
        if (f && fe) {
            char *body = (char *)malloc((size_t)(fe - f) + 1); memcpy(body, f, (size_t)(fe - f)); body[fe - f] = 0;
            char *al = NULL; { const char *g = strstr(m, "- (n48ia_act)n48IOAlias:(N48Texture *)t need:(BOOL)need write:(BOOL)w {"); const char *ge = g ? strstr(g, "\n}\n") : NULL; if (g && ge) { al = (char *)malloc((size_t)(ge - g) + 1); memcpy(al, g, (size_t)(ge - g)); al[ge - g] = 0; } }
            CHECK("source: the decision comes from n48ia_touch keyed by the (surface id, PLANE OFFSET) of n48AliasKey and by the path of the wrapper", al && strstr(al, "[t n48AliasKey:&sid off:&off]") && strstr(al, "n48ia_touch(&_ia, sid, off,") && strstr(al, "[t n48IOSLinear] ? 0 : 1"), "alias method");
            free(al);
            CHECK("source: n48AliasKey is (IOSurfaceGetID, _ioff) for a surface texture", strstr(m, "*sid = _ios ? (uint32_t)IOSurfaceGetID(_ios) : 0; *off = (uint64_t)_ioff;") != NULL, "key from surface id + plane offset");
            CHECK("source: a flush copies the writer back (n48_ios_writeback) and takes it out of the write-back list", strstr(body, "n48_ios_writeback(") && strstr(body, "n48IOUnwrite:"), "flush + unwrite");
            CHECK("source: the flush is followed by a full barrier (transfer write -> GPU read)", strstr(body, "n48_full_barrier(") != NULL, "barrier");
            CHECK("source: the upload comes after the flush in the function", strstr(body, "if (d.flush) {") && strstr(body, "if (d.upload) {") && strstr(body, "if (d.flush) {") < strstr(body, "if (d.upload) {"), "order");
            CHECK("source: WindowServer path: the legacy first-touch upload is kept and the case is only counted", strstr(body, "iaWsFlush") != NULL && strstr(body, "iaWsReup") != NULL && strstr(body, "if (first && need) {") != NULL && strstr(body, "if (!n48_ioalias_acts()) {") != NULL, "legacy + count");
            { const char *fl = strstr(body, "if (d.flush) {"), *up = strstr(body, "if (d.upload) {"); char fb[1200], ub[600]; size_t fn = fl && up ? (size_t)(up - fl) : 0; if (fn >= sizeof fb) fn = sizeof fb - 1; if (fl && up) memcpy(fb, fl, fn); fb[fn] = 0;
              size_t un = up ? strlen(up) : 0; if (un >= sizeof ub) un = sizeof ub - 1; if (up) memcpy(ub, up, un); ub[un] = 0;
              CHECK("source: the render pass is ended before the copy-back AND before the re-upload", fl && up && strstr(fb, "if (endPass) endPass();") && strstr(fb, "if (endPass) endPass();") < strstr(fb, "n48_ios_writeback(") && strstr(ub, "if (endPass) endPass();") && strstr(ub, "if (endPass) endPass();") < strstr(ub, "n48_ios_upload("), "endPass before each recorded copy"); }
            free(body);
        }
        CHECK("source: -n48IOUnwrite: exists", strstr(m, "- (void)n48IOUnwrite:(N48Texture *)t") != NULL, "defined");
        free(m);
    }
    printf("%s (%d failure%s)\n", fails ? "FAILED" : "ALL PASSED", fails, fails == 1 ? "" : "s");
    return fails != 0;
}
