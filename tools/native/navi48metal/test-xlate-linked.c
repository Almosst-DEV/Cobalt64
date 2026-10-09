// test-xlate-linked.c: P3 (build 18, an internal design note): linked functions in the in-process translation.
// Build/run (from this directory):  cc -O1 -Wall -Wextra -Werror -o /tmp/test-xlate-linked test-xlate-linked.c && /tmp/test-xlate-linked .      (argv[1] = the directory holding Navi48Device.m and n48_xlate.h)
// Covers: the cache key (n48x_link_sha) - what changes it and what must not; one job with dependencies (every dependency through the SAME reader, the library's linked entry point, the failure codes and
// caps); the engine request (the worker gets copies); and source pins that tie Navi48Device.m's glue to them (privateFunctions and groups are read and logged, binaryFunctions / preloaded libraries refuse,
// the lookup tries the linked key first, the key is built from the dependencies, the context is scoped).
#define _DARWIN_C_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <dirent.h>
#include "n48_xlate.h"

static int fails, runs;
#define CHECK(name, cond) do { int ok_ = (cond) ? 1 : 0; runs++; if (!ok_) { fails++; printf("FAIL: %s\n", name); } } while (0)
static char *slurp(const char *dir, const char *name) {
    char path[1024]; snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE *f = fopen(path, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char *b = malloc((size_t)n + 1); if (!b) { fclose(f); return NULL; }
    if (fread(b, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(b); return NULL; }
    b[n] = 0; fclose(f); return b;
}
static int before(const char *a, const char *b) { return a && b && a < b; }
static int has(const char *from, const char *to, const char *needle) { const char *q = from ? strstr(from, needle) : NULL; return q != NULL && (to == NULL || q < to); }
static const char *fn_start(const char *src, const char *sig) { return strstr(src, sig); }
static char TMP[512];
static void rmtree(const char *path) {
    struct stat st; if (lstat(path, &st) != 0) return;
    if (S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode)) {
        DIR *d = opendir(path); struct dirent *e;
        if (d) { while ((e = readdir(d)) != NULL) { if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue; char p[1400]; snprintf(p, sizeof p, "%s/%s", path, e->d_name); rmtree(p); } closedir(d); }
        rmdir(path);
    } else unlink(path);
}
static void cleanup(void) { if (strstr(TMP, "/n48xlate-linked.") && strlen(TMP) > 24) rmtree(TMP); }
static int file_exists(const char *dir, const char *sha, const char *ext) { char p[1100]; return n48x_file_path(p, sizeof p, dir, sha, ext) && n48x_exists(p); }

static const char SHA_A[] = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
static const char SHA_B[] = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
static const char SHA_C[] = "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc";
static const char SHA_D[] = "dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd";
static const char SHA_E[] = "eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee";

// ---- fakes: the reader prints "LL:<first bytes of the bitcode>", the linked translator records what it was given ----
static struct { int airCalls, plainCalls, linkedCalls, readerFailsAt, freeCalls; int nd; char sym[8][32]; char ll[8][64]; char entryLl[64]; } FK;
static void fk_reset(void) { memset(&FK, 0, sizeof FK); FK.readerFailsAt = -1; }
static int fk_air(void *c, const uint8_t *air, size_t n, char **t, size_t *l, char *e, size_t en) {
    (void)c; const int idx = FK.airCalls++;
    if (FK.readerFailsAt == idx) { snprintf(e, en, "fake reader refused call %d", idx); return -1; }
    char buf[80]; snprintf(buf, sizeof buf, "LL:%.*s", (int)(n < 8 ? n : 8), (const char *)air);
    *t = strdup(buf); *l = strlen(buf); return 0;
}
static void fk_free_text(void *c, char *t) { (void)c; FK.freeCalls++; free(t); }
static int fk_fill(uint8_t **spv, size_t *sl, uint8_t **meta, size_t *ml) {
    *sl = 64; *spv = calloc(1, 64); (*spv)[0] = 3; (*spv)[1] = 2; (*spv)[2] = 0x23; (*spv)[3] = 7;
    const char *m = "{\"stage\":\"Fragment\"}"; *ml = strlen(m); *meta = malloc(*ml); memcpy(*meta, m, *ml); return 0;
}
static int fk_xl(void *c, const char *ll, size_t llLen, const uint32_t *local, uint8_t **spv, size_t *sl, uint8_t **meta, size_t *ml, char *e, size_t en) {
    (void)c; (void)ll; (void)llLen; (void)local; (void)e; (void)en; FK.plainCalls++; return fk_fill(spv, sl, meta, ml);
}
static int fk_xl_linked(void *c, const char *ll, size_t llLen, const uint32_t *local, const n48x_ldep_ll *deps, size_t nd, uint8_t **spv, size_t *sl, uint8_t **meta, size_t *ml, char *e, size_t en) {
    (void)c; (void)llLen; (void)local; (void)e; (void)en; FK.linkedCalls++;
    snprintf(FK.entryLl, sizeof FK.entryLl, "%s", ll); FK.nd = (int)nd;
    for (size_t i = 0; i < nd && i < 8; i++) { snprintf(FK.sym[i], sizeof FK.sym[i], "%s", deps[i].symbol); snprintf(FK.ll[i], sizeof FK.ll[i], "%.*s", (int)deps[i].llLen, deps[i].ll); }
    return fk_fill(spv, sl, meta, ml);
}
static void fk_release(void *c, uint8_t *p, size_t n) { (void)c; (void)n; free(p); }
static const n48x_deps LDEPS = { NULL, fk_air, fk_free_text, fk_xl, fk_release, fk_xl_linked };
static const n48x_deps NOLINK = { NULL, fk_air, fk_free_text, fk_xl, fk_release, NULL };
static const uint8_t ENTRY_AIR[17] = "ENTRY---BC\xc0\xde----";
static const uint8_t DEP1_AIR[17] = "DEP1----BC\xc0\xde----";
static const uint8_t DEP2_AIR[17] = "DEP2----BC\xc0\xde----";

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : ".";
    {
        const char *base = getenv("N48_TEST_TMP"); if (!base) base = getenv("TMPDIR"); if (!base || !*base) base = "/private/tmp";
        snprintf(TMP, sizeof TMP, "%s/n48xlate-linked.XXXXXX", base);
        if (!mkdtemp(TMP)) { printf("cannot create a scratch dir under %s\n", base); return 2; }
        atexit(cleanup);
    }

    // ===== the cache key =====
    {
        char k0[65], k1[65], k2[65], k3[65], k4[65], k5[65], k6[65], k7[65];
        const char *sy[] = { "alpha", "beta" }, *sh[] = { SHA_B, SHA_C };
        CHECK("key: formed", n48x_link_sha(k0, SHA_A, 2, sy, sh) == 0 && n48x_sha_ok(k0));
        n48x_link_sha(k1, SHA_A, 2, sy, sh); CHECK("key: deterministic", !strcmp(k0, k1));
        const char *sy2[] = { "beta", "alpha" }, *sh2[] = { SHA_C, SHA_B };
        n48x_link_sha(k2, SHA_A, 2, sy2, sh2); CHECK("key: the order the client listed the functions in does not matter", !strcmp(k0, k2));
        const char *sy3[] = { "alpha", "beta", "alpha" }, *sh3[] = { SHA_B, SHA_C, SHA_B };
        n48x_link_sha(k3, SHA_A, 3, sy3, sh3); CHECK("key: a function listed twice (same name, same body) does not change it", !strcmp(k0, k3));
        n48x_link_sha(k4, SHA_D, 2, sy, sh); CHECK("key: a different ENTRY gives a different key", strcmp(k0, k4) != 0);
        const char *sh5[] = { SHA_B, SHA_D }; n48x_link_sha(k5, SHA_A, 2, sy, sh5); CHECK("key: a different dependency BODY gives a different key (the dependency decides the output)", strcmp(k0, k5) != 0);
        const char *sy6[] = { "alpha", "gamma" }; n48x_link_sha(k6, SHA_A, 2, sy6, sh); CHECK("key: a different dependency NAME gives a different key", strcmp(k0, k6) != 0);
        n48x_link_sha(k7, SHA_A, 1, sy, sh); CHECK("key: a removed dependency gives a different key", strcmp(k0, k7) != 0);
        char kn[65]; n48x_link_sha(kn, SHA_A, 0, NULL, NULL); CHECK("key: no dependencies is still a key, and not the entry's own sha", n48x_sha_ok(kn) && strcmp(kn, SHA_A) != 0 && strcmp(kn, k0) != 0);
        const char *sy8[] = { "alpha", "alpha" }, *sh8[] = { SHA_B, SHA_C }; char k8[65]; n48x_link_sha(k8, SHA_A, 2, sy8, sh8);
        CHECK("key: one name linked to two different bodies is two dependencies, not one", strcmp(k8, k7) != 0 && strcmp(k8, k0) != 0);
        CHECK("key: a malformed sha or an empty name is refused", n48x_link_sha(k0, "zz", 0, NULL, NULL) != 0 && k0[0] == 0 && ({ const char *e1[] = { "" }; const char *e2[] = { SHA_B }; n48x_link_sha(k0, SHA_A, 1, e1, e2); }) != 0);
        // adjacency: ("ab","c...") and ("a","bc...") must not collide
        const char *sa[] = { "ab" }, *sb[] = { "a" }, *sx[] = { SHA_B }; char ka[65], kb[65]; n48x_link_sha(ka, SHA_A, 1, sa, sx); n48x_link_sha(kb, SHA_A, 1, sb, sx);
        CHECK("key: the name is delimited", strcmp(ka, kb) != 0);
    }

    // ===== one job with dependencies =====
    {
        char d[600]; snprintf(d, sizeof d, "%s/job", TMP); mkdir(d, 0700); char err[300];
        char key[65]; const char *sy[] = { "dep1", "dep2" }, *sh[] = { SHA_B, SHA_C }; n48x_link_sha(key, SHA_A, 2, sy, sh);
        n48x_ldep ld[2] = { { "dep1", DEP1_AIR, sizeof DEP1_AIR }, { "dep2", DEP2_AIR, sizeof DEP2_AIR } };
        fk_reset();
        int rc = n48x_translate_job_x(&LDEPS, d, key, "frag", ENTRY_AIR, sizeof ENTRY_AIR, ld, 2, err, sizeof err);
        CHECK("job: ok with two dependencies", rc == N48X_OK && file_exists(d, key, N48X_EXT_SPV) && file_exists(d, key, N48X_EXT_META));
        CHECK("job: the linked entry point was used (and not the plain one)", FK.linkedCalls == 1 && FK.plainCalls == 0);
        CHECK("job: the entry and EVERY dependency went through the reader (3 calls)", FK.airCalls == 3);
        CHECK("job: the library got the dependencies' names and READ text, in order", FK.nd == 2 && !strcmp(FK.sym[0], "dep1") && !strcmp(FK.sym[1], "dep2") && !strncmp(FK.ll[0], "LL:DEP1", 7) && !strncmp(FK.ll[1], "LL:DEP2", 7) && !strncmp(FK.entryLl, "LL:ENTRY", 8));
        CHECK("job: every text buffer was released (entry + 2)", FK.freeCalls == 3);
        CHECK("job: the result is stored under the LINKED key, not the entry's sha", !file_exists(d, SHA_A, N48X_EXT_SPV));
        CHECK("job: no marker is left", !file_exists(d, key, N48X_EXT_INFLIGHT));
        // reader refuses the second dependency
        fk_reset(); FK.readerFailsAt = 2;
        rc = n48x_translate_job_x(&LDEPS, d, SHA_D, "frag", ENTRY_AIR, sizeof ENTRY_AIR, ld, 2, err, sizeof err);
        CHECK("job: a dependency the reader refuses is a READER refusal with a .fail and no result", rc == N48X_E_READER && file_exists(d, SHA_D, N48X_EXT_FAIL) && !file_exists(d, SHA_D, N48X_EXT_SPV) && FK.linkedCalls == 0);
        CHECK("job: the text already read was released on the failure path (entry + first dependency)", FK.freeCalls == 2);
        CHECK("job: the marker is removed on that path", !file_exists(d, SHA_D, N48X_EXT_INFLIGHT));
        // no linked entry point in the library
        fk_reset();
        rc = n48x_translate_job_x(&NOLINK, d, SHA_E, "frag", ENTRY_AIR, sizeof ENTRY_AIR, ld, 2, err, sizeof err);
        CHECK("job: dependencies but no linked entry point = UNAVAILABLE, never a silent plain translation", rc == N48X_E_UNAVAILABLE && FK.plainCalls == 0 && FK.linkedCalls == 0 && !file_exists(d, SHA_E, N48X_EXT_SPV));
        // none
        fk_reset();
        rc = n48x_translate_job_x(&LDEPS, d, SHA_B, "frag", ENTRY_AIR, sizeof ENTRY_AIR, NULL, 0, err, sizeof err);
        CHECK("job: no dependencies = the plain entry point (every earlier caller)", rc == N48X_OK && FK.plainCalls == 1 && FK.linkedCalls == 0 && FK.airCalls == 1);
        fk_reset(); rc = n48x_translate_job(&LDEPS, d, SHA_C, "frag", ENTRY_AIR, sizeof ENTRY_AIR, err, sizeof err);
        CHECK("job: the old signature is the unlinked job", rc == N48X_OK && FK.plainCalls == 1 && FK.linkedCalls == 0);
        // caps
        fk_reset(); n48x_ldep bad[1] = { { "dep1", DEP1_AIR, 2 } };
        char dd[600]; snprintf(dd, sizeof dd, "%s/job2", TMP); mkdir(dd, 0700);
        rc = n48x_translate_job_x(&LDEPS, dd, SHA_A, "frag", ENTRY_AIR, sizeof ENTRY_AIR, bad, 1, err, sizeof err);
        CHECK("job: a dependency with a bitcode size outside 4 bytes..4 MiB is a CAP refusal", rc == N48X_E_CAP && FK.linkedCalls == 0);
        fk_reset(); n48x_ldep noname[1] = { { "", DEP1_AIR, sizeof DEP1_AIR } };
        rc = n48x_translate_job_x(&LDEPS, dd, SHA_B, "frag", ENTRY_AIR, sizeof ENTRY_AIR, noname, 1, err, sizeof err);
        CHECK("job: a dependency without a name is a CAP refusal", rc == N48X_E_CAP && FK.linkedCalls == 0);
        fk_reset(); n48x_ldep many[N48X_MAX_DEPS + 1]; for (size_t i = 0; i <= N48X_MAX_DEPS; i++) { many[i].symbol = "d"; many[i].air = DEP1_AIR; many[i].airLen = sizeof DEP1_AIR; }
        rc = n48x_translate_job_x(&LDEPS, dd, SHA_C, "frag", ENTRY_AIR, sizeof ENTRY_AIR, many, N48X_MAX_DEPS + 1, err, sizeof err);
        CHECK("job: more than N48X_MAX_DEPS dependencies is refused before any work", rc != N48X_OK && FK.linkedCalls == 0);
        // a .fail from an earlier refusal is honoured for the linked key too
        fk_reset(); rc = n48x_translate_job_x(&LDEPS, d, SHA_D, "frag", ENTRY_AIR, sizeof ENTRY_AIR, ld, 2, err, sizeof err);
        CHECK("job: a linked key that failed before is not retried (.fail)", rc == N48X_E_SKIP_FAIL && FK.airCalls == 0);
    }

    // ===== the engine request =====
    {
        char d[600]; snprintf(d, sizeof d, "%s/eng", TMP); mkdir(d, 0700); char err[300];
        static n48x_engine eng; fk_reset(); n48x_engine_init(&eng, &LDEPS, NULL, NULL, NULL, N48X_WORKER_STACK);
        char *sym = strdup("dep1"); uint8_t *air = malloc(sizeof DEP1_AIR); memcpy(air, DEP1_AIR, sizeof DEP1_AIR);
        n48x_ldep ld[1] = { { sym, air, sizeof DEP1_AIR } };
        int rc = n48x_engine_run_linked(&eng, d, SHA_A, "frag", ENTRY_AIR, sizeof ENTRY_AIR, ld, 1, 5000, err, sizeof err);
        CHECK("engine: a linked request runs on the worker and installs under the key", rc == N48X_OK && file_exists(d, SHA_A, N48X_EXT_SPV) && FK.linkedCalls == 1 && FK.nd == 1 && !strcmp(FK.sym[0], "dep1"));
        // the worker gets COPIES: the caller may free its arrays right after a timed-out wait
        fk_reset();
        char *sym2 = strdup("dep1"); uint8_t *air2 = malloc(sizeof DEP1_AIR); memcpy(air2, DEP1_AIR, sizeof DEP1_AIR);
        n48x_ldep ld2[1] = { { sym2, air2, sizeof DEP1_AIR } };
        rc = n48x_engine_run_linked(&eng, d, SHA_B, "frag", ENTRY_AIR, sizeof ENTRY_AIR, ld2, 1, 0, err, sizeof err);
        memset(air2, 'X', sizeof DEP1_AIR); free(air2); free(sym2);   // the request did not wait; the job copied the dependency
        CHECK("engine: a request that does not wait reports TIMEOUT", rc == N48X_E_TIMEOUT);
        for (int i = 0; i < 100 && !file_exists(d, SHA_B, N48X_EXT_SPV); i++) usleep(20000);
        CHECK("engine: ... and the worker still finished it from its own copy (the reader saw the original bytes)", file_exists(d, SHA_B, N48X_EXT_SPV) && FK.nd == 1 && !strncmp(FK.ll[0], "LL:DEP1", 7));
        fk_reset(); rc = n48x_engine_run(&eng, d, SHA_C, "frag", ENTRY_AIR, sizeof ENTRY_AIR, 5000, err, sizeof err);
        CHECK("engine: the old request is the unlinked one", rc == N48X_OK && FK.plainCalls == 1 && FK.linkedCalls == 0);
        n48x_ldep bad[1] = { { "x", DEP1_AIR, 1 } };
        rc = n48x_engine_run_linked(&eng, d, SHA_E, "frag", ENTRY_AIR, sizeof ENTRY_AIR, bad, 1, 100, err, sizeof err);
        CHECK("engine: a dependency over the caps is refused by the request", rc == N48X_E_CAP);
        free(sym); free(air);
    }

    // ===== source pins on the glue =====
    {
        char *src = slurp(dir, "Navi48Device.m");
        CHECK("pin: Navi48Device.m is readable", src != NULL);
        if (src) {
            const char *lc = fn_start(src, "static NSArray<NSDictionary *> *n48x_link_collect(id linked, NSString **why) {");
            const char *lcEnd = lc ? strstr(lc, "\n}\n") : NULL;
            CHECK("pin: the dependencies are collected from functions, privateFunctions AND groups", has(lc, lcEnd, "n48x_prop(linked, \"functions\")") && has(lc, lcEnd, "n48x_prop(linked, \"privateFunctions\")") && has(lc, lcEnd, "n48x_prop(linked, \"groups\")"));
            CHECK("pin: a name linked to two bodies, a missing name / bitcode and the caps all refuse", has(lc, lcEnd, "linked to two different functions") && has(lc, lcEnd, "has no name or no bitcodeData") && has(lc, lcEnd, "N48X_MAX_DEPS") && has(lc, lcEnd, "N48X_MAX_DEP_AIR"));
            const char *lk = fn_start(src, "static void n48x_linkage_check(id desc, const char *role, const char *linkedProp, const char *preloadProp, const char *name, id fn, int slot) {");
            const char *lkEnd = lk ? strstr(lk, "\n}\n") : NULL;
            CHECK("pin: binaryFunctions and preloaded libraries REFUSE (today's behaviour is kept)", has(lk, lkEnd, "if (bf || pl) why = @\"binaryFunctions or preloaded libraries are present\";"));
            CHECK("pin: privateFunctions and groups are read and logged with their names", has(lk, lkEnd, "n48x_array_nonempty(linked, \"privateFunctions\")") && has(lk, lkEnd, "n48x_names(n48x_prop(linked, \"privateFunctions\"), NO)") && has(lk, lkEnd, "n48x_names(gr, YES)") && has(lk, lkEnd, "\"groups\""));
            CHECK("pin: the cache key is built from the entry sha AND the sorted dependencies (n48x_link_sha over the collected list)", has(lk, lkEnd, "n48x_link_sha(key, esha.UTF8String, deps.count, syms, shas)") && has(lk, lkEnd, "shas[i] = [deps[i][@\"sha\"] UTF8String]") && has(lk, lkEnd, "syms[i] = [deps[i][@\"name\"] UTF8String]"));
            CHECK("pin: only a process that translates in process resolves linkage", has(lk, lkEnd, "else if (!n48x_active()) why = @\"this process does not translate in process\";"));
            CHECK("pin: the old LINKAGE IGNORED line still reports a refusal, with its reason", has(lk, lkEnd, "N48LOG(\"LINKAGE IGNORED %s '%s': "));
            const char *lr = fn_start(src, "static void n48x_linkage_render(id d) {");
            const char *lrEnd = lr ? strstr(lr, "\n}\n") : NULL;
            CHECK("pin: the render stages take slots 0 and 1, the kernel slot 2", has(lr, lrEnd, "vf, 0);") && has(lr, lrEnd, "ff, 1);") && has(src, NULL, "cf, 2);"));
            const char *tf = fn_start(src, "static BOOL n48x_translate_fn(id fn, const char *role, uint64_t deadlineNs) {");
            const char *tfEnd = tf ? strstr(tf, "\n}\n") : NULL;
            CHECK("pin: the translation request of a linked stage uses the linked key and sends the dependencies", has(tf, tfEnd, "if (lk) sha = lk->lsha;") && has(tf, tfEnd, "n48x_engine_run_linked(&N48XL.eng") && has(tf, tfEnd, "ld[i].air = (const uint8_t *)[dd[@\"bc\"] bytes]"));
            const char *sl = fn_start(src, "static NSData *n48_spv_lookup_impl(id fn, const char *role, NSError **err, NSString **fname, NSDictionary **meta) {");
            const char *slEnd = sl ? strstr(sl, "\n}\n") : NULL;
            CHECK("pin: the lookup tries the LINKED key first, then the entry's own sha", has(sl, slEnd, "(lk ? @[ lk->lsha, sha ] : @[ sha ])"));
            CHECK("pin: the meta sidecar and the touch use the key that HIT", has(sl, slEnd, "if (spv) { sha = cs; break; }") && before(strstr(sl, "if (spv) { sha = cs; break; }"), strstr(sl, "n48_meta_for(sha)")));
            const char *fs = fn_start(src, "static NSString *n48_fn_sha(id fn) {");
            const char *fsEnd = fs ? strstr(fs, "\n}\n") : NULL;
            CHECK("pin: the hot-swap registration of a linked stage watches the linked key", has(fs, fsEnd, "N48LinkCtx *lk_ = n48_lk_for(fn); if (lk_) return lk_->lsha;"));
            const char *np = fn_start(src, "- (id)n48NewPipeline:(MTLRenderPipelineDescriptor *)d error:(NSError **)error {");
            const char *npEnd = np ? strstr(np, "\n}\n") : NULL;
            CHECK("pin: the render creation installs the link contexts under a scope that clears them (before the pipeline is created)", before(strstr(np, "N48_LK_SCOPE;"), strstr(np, "n48x_linkage_render(d);")) && before(strstr(np, "n48x_linkage_render(d);"), strstr(np, "initWithDevice:self descriptor:d")) && npEnd);
            const char *nc = fn_start(src, "- (id)n48NewComputePipeline:(id)fn error:(NSError **)error {");
            const char *ncEnd = nc ? strstr(nc, "\n}\n") : NULL;
            CHECK("pin: the compute creation clears the contexts at every return", has(nc, ncEnd, "N48_LK_SCOPE;") && has(src, NULL, "#define N48_LK_SCOPE n48_lkscope lks_ __attribute__((cleanup(n48_lk_scope_end)))"));
            CHECK("pin: the four compute descriptor entry points install the kernel's context first", ({ int n = 0; const char *q = src; while ((q = strstr(q, "n48x_linkage_compute(d); N48LOG(\"newComputePipelineStateWithDescriptor:"))) { n++; q++; } n == 4; }));
            CHECK("pin: the hot rebuild of a render pipeline and of a compute placeholder re-install the contexts on the watcher thread", has(src, NULL, "N48_LK_SCOPE; n48x_linkage_render(_hsDesc);") && has(src, NULL, "N48_LK_SCOPE; if (_hsLk) n48_lk_install(2, _hsLk);"));
            free(src);
        }
        char *hdr = slurp(dir, "n48_xlate.h");
        CHECK("pin: n48_xlate.h is readable", hdr != NULL);
        if (hdr) {
            const char *tj = fn_start(hdr, "static inline int n48x_translate_job_x(");
            const char *tjEnd = tj ? strstr(tj, "\n}\n") : NULL;
            const char *r1 = tj ? strstr(tj, "dp->air_to_text(dp->ctx, air, airLen") : NULL, *r2 = tj ? strstr(tj, "dp->air_to_text(dp->ctx, ld[i].air") : NULL, *t1 = tj ? strstr(tj, "dp->translate_linked(") : NULL;
            CHECK("pin: the entry is read, then each dependency (with the same reader), then the linked translation", before(r1, r2) && before(r2, t1) && t1 < tjEnd);
            CHECK("pin: ABI 2 is the version the bundle accepts, and the library must export the linked entry point", has(hdr, NULL, "#define N48X_ABI_VERSION   2u") && has(hdr, NULL, "dlsym(h, \"n48x_translate_linked\")"));
            free(hdr);
        }
    }
    printf("test-xlate-linked: %d checks, %d failed\n", runs, fails);
    return fails ? 1 : 0;
}
