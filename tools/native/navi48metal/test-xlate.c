// test-xlate.c: n48_xlate.h (bundle 14, in-process shader translation; an internal design note section 1) on the host, plus source pins that tie the REAL Navi48Device.m to it.
// Build/run on the host Mac (from this directory; argv[1] = the directory holding Navi48Device.m and n48_xlate.h, default "."; argv[2] = autotranslate/overrides.txt, default ../autotranslate/overrides.txt):
//   cc -O1 -Wall -Wextra -Werror -DN48X_TESTHOOKS -o /tmp/test-xlate test-xlate.c && /tmp/test-xlate .
// test-b14-plant.sh plants breaks in the REAL n48_xlate.h, Navi48Device.m and the fork's Rust and demands that this suite (or the fork's cargo tests) FAIL on each.
// Covers:
//   T1  every combination of n48x_applies; the kill-file and environment readers; the side-directory rule; the cache key (every part of it matters) and the path builders; the size caps; the crash-marker
//       plan table and the real marker files (create, probe, promote, sweep); the least-recently-used trim (the pure decision and the real directory); the override table against overrides.txt;
//       the Mach-O UUID reader; the reader's candidate order; the worker thread's stack; one job's order of operations (marker, meta before spv, .fail, caps); the engine (timeout, sharing, late install)
//   T4  crash guard: a child process that dies mid-translation (the test-only _exit hook) leaves the marker; the next run skips the sha and never calls the translator; a new key retries
//   G   source pins on Navi48Device.m: the gate call, the lookup order, the side-directory skip, the worker stack, the order of the miss branches, the LINKAGE IGNORED lines
// (T2/T3/T5 are the fork's cargo tests and test-xlate-corpus.sh.)
#define _DARWIN_C_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <mach-o/dyld.h>
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
static const char *fn_start(const char *src, const char *sig) { return strstr(src, sig); }
static int before(const char *a, const char *b) { return a && b && a < b; }
static int has(const char *from, const char *to, const char *needle) { const char *q = from ? strstr(from, needle) : NULL; return q != NULL && (to == NULL || q < to); }

static char TMP[512];   // scratch root, removed at exit (prefix-checked)
static void rmtree(const char *path) {   // plain files and directories only; never follows a link
    struct stat st; if (lstat(path, &st) != 0) return;
    if (S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode)) {
        DIR *d = opendir(path); struct dirent *e;
        if (d) { while ((e = readdir(d)) != NULL) { if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue; char p[1400]; snprintf(p, sizeof p, "%s/%s", path, e->d_name); rmtree(p); } closedir(d); }
        rmdir(path);
    } else unlink(path);
}
static void cleanup(void) { if (strstr(TMP, "/n48xlate-test.") && strlen(TMP) > 20) rmtree(TMP); }
static void mkd(const char *p) { mkdir(p, 0700); }
static void touch_file(const char *p, const char *content) { FILE *f = fopen(p, "wb"); if (f) { fputs(content, f); fclose(f); } }
static void set_mtime(const char *p, long secs) { struct timeval tv[2]; tv[0].tv_sec = tv[1].tv_sec = secs; tv[0].tv_usec = tv[1].tv_usec = 0; utimes(p, tv); }
static const char SHA_A[] = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
static const char SHA_B[] = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
static const char SHA_C[] = "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc";
static const char SHA_D[] = "dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd";

// ---- fake dependencies ----
static struct {
    int airCalls, xlCalls, relCalls, freeCalls;
    int readerFails, translateRc, bigText, bigSpv, delayMs, markerSeenInReader, markerSeenInXlate;
    const char *dir, *sha; size_t stackSeen;
} FK;
static void fk_reset(void) { memset(&FK, 0, sizeof FK); }
static int fk_air(void *c, const uint8_t *air, size_t n, char **t, size_t *l, char *e, size_t en) {
    (void)c; (void)air; (void)n; FK.airCalls++;
    FK.stackSeen = pthread_get_stacksize_np(pthread_self());
    if (FK.dir && FK.sha) { char p[1100]; n48x_file_path(p, sizeof p, FK.dir, FK.sha, N48X_EXT_INFLIGHT); if (n48x_exists(p)) FK.markerSeenInReader = 1; }
    if (FK.delayMs) usleep((useconds_t)FK.delayMs * 1000);
    if (FK.readerFails) { snprintf(e, en, "fake reader refused"); return -1; }
    *t = strdup("; fake ll\n"); *l = FK.bigText ? (size_t)N48X_MAX_LL + 1 : strlen(*t); return 0;
}
static void fk_free_text(void *c, char *t) { (void)c; FK.freeCalls++; free(t); }
static int fk_xl(void *c, const char *ll, size_t llLen, const uint32_t *local, uint8_t **spv, size_t *sl, uint8_t **meta, size_t *ml, char *e, size_t en) {
    (void)c; (void)ll; (void)llLen; (void)local; FK.xlCalls++;
    if (FK.dir && FK.sha) { char p[1100]; n48x_file_path(p, sizeof p, FK.dir, FK.sha, N48X_EXT_INFLIGHT); if (n48x_exists(p)) FK.markerSeenInXlate = 1; }
    if (FK.translateRc) { snprintf(e, en, "fake translator rc %d", FK.translateRc); return FK.translateRc; }
    *sl = FK.bigSpv ? (size_t)N48X_MAX_SPV + 4 : 64; *spv = calloc(1, *sl ? *sl : 1); if (FK.bigSpv) *sl = (size_t)N48X_MAX_SPV + 4;
    (*spv)[0] = 3; (*spv)[1] = 2; (*spv)[2] = 0x23; (*spv)[3] = 7;
    const char *m = "{\"stage\":\"Kernel\"}"; *ml = strlen(m); *meta = malloc(*ml); memcpy(*meta, m, *ml);
    return 0;
}
static void fk_release(void *c, uint8_t *p, size_t n) { (void)c; (void)n; FK.relCalls++; free(p); }
static const n48x_deps FDEPS = { NULL, fk_air, fk_free_text, fk_xl, fk_release, NULL };
static int fk_job(const char *dir, const char *sha, const char *name, char *err, size_t en) {
    static const uint8_t air[16] = "BC\xc0\xde-----------";
    return n48x_translate_job(&FDEPS, dir, sha, name, air, sizeof air, err, en);
}
static int file_exists(const char *dir, const char *sha, const char *ext) { char p[1100]; return n48x_file_path(p, sizeof p, dir, sha, ext) && n48x_exists(p); }

// ---- the worker-stack probe (T1: the real engine must hand its worker 64 MiB) ----
static int stack_air(void *c, const uint8_t *air, size_t n, char **t, size_t *l, char *e, size_t en) { FK.stackSeen = pthread_get_stacksize_np(pthread_self()); return fk_air(c, air, n, t, l, e, en); }

struct share { n48x_engine *e; const char *d; int rc; };
static void *share_thread(void *a) {   // a second caller, shortly after the first, asking for the same sha
    struct share *sp = (struct share *)a; static const uint8_t air2[16] = "BC\xc0\xde-----------"; char er[100];
    usleep(60000); sp->rc = n48x_engine_run(sp->e, sp->d, SHA_D, "k", air2, sizeof air2, 5000, er, sizeof er); return NULL;
}
static size_t seen_stack;
static void *probe_stack(void *a) { (void)a; seen_stack = pthread_get_stacksize_np(pthread_self()); return NULL; }

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : ".";
    char ovrPath[1024]; snprintf(ovrPath, sizeof ovrPath, "%s", argc > 2 ? argv[2] : "../autotranslate/overrides.txt");
    {   // scratch root: under $TMPDIR (or /private/tmp)
        const char *base = getenv("N48_TEST_TMP"); if (!base) base = getenv("TMPDIR"); if (!base || !*base) base = "/private/tmp";
        snprintf(TMP, sizeof TMP, "%s/n48xlate-test.XXXXXX", base);
        if (!mkdtemp(TMP)) { printf("cannot create a scratch dir under %s\n", base); return 2; }
        atexit(cleanup);
    }

    // ===== T1.1 the gate: every combination =====
    {
        int bad = 0, n = 0;
        for (int m = 0; m < 64; m++) {
            const int isWs = m & 1, admitted = (m >> 1) & 1, rootAllow = (m >> 2) & 1, testInproc = (m >> 3) & 1, killed = (m >> 4) & 1, envDirty = (m >> 5) & 1;
            int want;   // written out as a table, not as the implementation's expression
            if (isWs) want = 0; else if (killed) want = 0; else if (envDirty) want = 0; else if (rootAllow) want = testInproc; else want = admitted;
            if (n48x_applies(isWs, admitted, rootAllow, testInproc, killed, envDirty) != want) bad++;
            n++;
        }
        CHECK("n48x_applies: all 64 combinations match the table", bad == 0 && n == 64);
        CHECK("n48x_applies: WindowServer is never in-process, whatever else is set", !n48x_applies(1, 1, 1, 1, 0, 0) && !n48x_applies(1, 1, 0, 0, 0, 0) && !n48x_applies(1, 0, 1, 1, 0, 0));
        CHECK("n48x_applies: an unadmitted ordinary process is not in-process", !n48x_applies(0, 0, 0, 0, 0, 0) && !n48x_applies(0, 0, 0, 1, 0, 0));
        CHECK("n48x_applies: an admitted application is", n48x_applies(0, 1, 0, 0, 0, 0));
        CHECK("n48x_applies: a root tool needs N48M_TEST_INPROC", !n48x_applies(0, 0, 1, 0, 0, 0) && n48x_applies(0, 0, 1, 1, 0, 0));
        CHECK("n48x_applies: the kill file turns an admitted application off", !n48x_applies(0, 1, 0, 0, 1, 0) && !n48x_applies(0, 0, 1, 1, 1, 0));
        CHECK("n48x_applies: a METAL2VULKAN_ variable turns an admitted application off", !n48x_applies(0, 1, 0, 0, 0, 1) && !n48x_applies(0, 0, 1, 1, 0, 1));
    }
    {
        char *e0[] = { NULL };
        char *e1[] = { (char *)"PATH=/usr/bin", (char *)"HOME=/x", NULL };
        char *e2[] = { (char *)"PATH=/usr/bin", (char *)"METAL2VULKAN_WHOLE_PART=1", NULL };
        char *e3[] = { (char *)"METAL2VULKAN", (char *)"XMETAL2VULKAN_A=1", (char *)"metal2vulkan_a=1", NULL };
        char *e4[] = { (char *)"METAL2VULKAN_", NULL };
        CHECK("env: nothing / unrelated variables are clean", !n48x_env_dirty(e0) && !n48x_env_dirty(e1) && !n48x_env_dirty(NULL));
        CHECK("env: a METAL2VULKAN_ variable is dirty, wherever it sits", n48x_env_dirty(e2) && n48x_env_dirty(e4));
        CHECK("env: near misses (no underscore, a prefix, lower case) are clean", !n48x_env_dirty(e3));
        CHECK("kill file: absent (ENOENT / ENOTDIR) is not killed", !n48x_killed_from_stat(-1, ENOENT) && !n48x_killed_from_stat(-1, ENOTDIR));
        CHECK("kill file: present is killed", n48x_killed_from_stat(0, 0) == 1);
        CHECK("kill file: unreadable (EPERM / EACCES / other) is killed, fail closed", n48x_killed_from_stat(-1, EPERM) && n48x_killed_from_stat(-1, EACCES) && n48x_killed_from_stat(-1, EIO));
        CHECK("side directory: skipped for a sandboxed application, kept for WindowServer and for unsandboxed processes",
              n48x_skip_side_dir(0, 1) == 1 && n48x_skip_side_dir(1, 1) == 0 && n48x_skip_side_dir(0, 0) == 0 && n48x_skip_side_dir(1, 0) == 0);
    }

    // ===== T1.2 the cache key =====
    {
        uint8_t xl[16], rd[16], ov[32]; memset(xl, 1, 16); memset(rd, 2, 16); n48x_override_hash(ov);
        char k0[17], k1[17], k2[17], k3[17], k4[17], k5[17];
        n48x_key(k0, xl, rd, ov, "26A434"); n48x_key(k1, xl, rd, ov, "26A434");
        CHECK("key: deterministic, 16 lowercase hex digits", !strcmp(k0, k1) && n48x_key_ok(k0) && strlen(k0) == 16);
        uint8_t xl2[16]; memcpy(xl2, xl, 16); xl2[15] ^= 1; n48x_key(k2, xl2, rd, ov, "26A434");
        uint8_t rd2[16]; memcpy(rd2, rd, 16); rd2[7] ^= 0x80; n48x_key(k3, xl, rd2, ov, "26A434");
        uint8_t ov2[32]; memcpy(ov2, ov, 32); ov2[0] ^= 1; n48x_key(k4, xl, rd, ov2, "26A434");
        n48x_key(k5, xl, rd, ov, "26A435");
        CHECK("key: a new translator build (LC_UUID of libn48xlate) is a new key", strcmp(k0, k2) != 0);
        CHECK("key: a new reader library (its UUID) is a new key", strcmp(k0, k3) != 0);
        CHECK("key: a changed override table is a new key", strcmp(k0, k4) != 0);
        CHECK("key: a new OS build (kern.osversion) is a new key", strcmp(k0, k5) != 0);
        char k6[17], k7[17]; uint8_t z[16]; memset(z, 0, 16); n48x_key(k6, xl, z, ov, "26A434"); n48x_key(k7, z, rd, ov, "26A434");
        CHECK("key: the translator and the reader identities are not interchangeable", strcmp(k6, k7) != 0 && strcmp(k6, k0) != 0);
        uint8_t ov3[32]; n48x_override_hash(ov3);
        CHECK("key: the override hash is stable and non-zero", !memcmp(ov, ov3, 32) && memcmp(ov, z, 16) != 0);
        CHECK("key: a NULL osversion is the empty string, not a crash", ({ char kk[17]; n48x_key(kk, xl, rd, ov, NULL); n48x_key_ok(kk); }));
        CHECK("n48x_key_ok: rejects upper case, short, long, non-hex", !n48x_key_ok("ABCDEF0123456789") && !n48x_key_ok("abc") && !n48x_key_ok("0123456789abcdef0") && !n48x_key_ok("0123456789abcdeg") && !n48x_key_ok(NULL));
    }

    // ===== T1.3 paths =====
    {
        char p[1100], q[1100];
        CHECK("parent path: <ucd>/com.navi48.xlate (ucd ends with '/')", n48x_parent_path(p, sizeof p, "/var/folders/xx/C/") && !strcmp(p, "/var/folders/xx/C/com.navi48.xlate"));
        CHECK("parent path: a user cache dir without the slash gets one", n48x_parent_path(p, sizeof p, "/var/folders/xx/C") && !strcmp(p, "/var/folders/xx/C/com.navi48.xlate"));
        CHECK("parent path: no user cache dir -> none", n48x_parent_path(p, sizeof p, NULL) == 0 && n48x_parent_path(p, sizeof p, "") == 0 && p[0] == 0);
        CHECK("dir path: <ucd>/com.navi48.xlate/<K>", n48x_dir_path(p, sizeof p, "/c/", "0123456789abcdef") && !strcmp(p, "/c/com.navi48.xlate/0123456789abcdef"));
        CHECK("dir path: a bad key makes no path", n48x_dir_path(p, sizeof p, "/c/", "nothex") == 0 && p[0] == 0);
        CHECK("dir path: too small a buffer -> none", n48x_dir_path(q, 12, "/c/", "0123456789abcdef") == 0 && q[0] == 0);
        CHECK("file path: <dir>/<sha><ext> for each extension", n48x_file_path(p, sizeof p, "/d", SHA_A, N48X_EXT_SPV) && !strcmp(p + strlen(p) - 4, ".spv") && strstr(p, "/d/aaaa") == p &&
              n48x_file_path(p, sizeof p, "/d", SHA_A, N48X_EXT_META) && !strcmp(p + strlen(p) - 10, ".meta.json") &&
              n48x_file_path(p, sizeof p, "/d", SHA_A, N48X_EXT_FAIL) && n48x_file_path(p, sizeof p, "/d", SHA_A, N48X_EXT_INFLIGHT) && n48x_file_path(p, sizeof p, "/d", SHA_A, N48X_EXT_CRASH));
        CHECK("file path: a path-traversal or short sha makes no path", n48x_file_path(p, sizeof p, "/d", "../../etc/passwd", ".spv") == 0 && n48x_file_path(p, sizeof p, "/d", "abc", ".spv") == 0 && n48x_file_path(p, sizeof p, "/d", NULL, ".spv") == 0);
        char upper[65]; memset(upper, 'A', 64); upper[64] = 0;
        CHECK("sha_ok: lower-case hex of exactly 64", n48x_sha_ok(SHA_A) && !n48x_sha_ok(upper) && !n48x_sha_ok("") && !n48x_sha_ok(NULL));
    }

    // ===== T1.4 caps =====
    CHECK("caps: bitcode 4 bytes .. 4 MiB", !n48x_air_size_ok(0) && !n48x_air_size_ok(3) && n48x_air_size_ok(4) && n48x_air_size_ok(4u << 20) && !n48x_air_size_ok((4u << 20) + 1));
    CHECK("caps: LLVM text 1 .. 64 MiB", !n48x_ll_size_ok(0) && n48x_ll_size_ok(1) && n48x_ll_size_ok(64u << 20) && !n48x_ll_size_ok((64u << 20) + 1));
    CHECK("caps: SPIR-V >= 20 bytes, word aligned, <= 16 MiB", !n48x_spv_size_ok(16) && !n48x_spv_size_ok(22) && n48x_spv_size_ok(20) && n48x_spv_size_ok(16u << 20) && !n48x_spv_size_ok((16u << 20) + 4));
    CHECK("caps: the documented sizes", N48X_MAX_AIR == 4u << 20 && N48X_MAX_LL == 64u << 20 && N48X_MAX_SPV == 16u << 20 && N48X_CACHE_CAP == 256ull << 20 && N48X_WORKER_STACK == (size_t)64u << 20);

    // ===== T1.5 the plan table =====
    {
        int bad = 0;
        for (int m = 0; m < 16; m++) {
            const int crash = m & 1, fail = (m >> 1) & 1, infl = (m >> 2) & 1, alive = (m >> 3) & 1;
            int want;   // the order of precedence, written out
            if (crash) want = N48X_PL_SKIP_CRASH;
            else if (infl && alive) want = N48X_PL_SKIP_LIVE;
            else if (infl && !alive) want = N48X_PL_PROMOTE_THEN_SKIP;
            else if (fail) want = N48X_PL_SKIP_FAIL;
            else want = N48X_PL_TRANSLATE;
            if (n48x_plan(crash, fail, infl, alive) != want) bad++;
        }
        CHECK("plan: all 16 combinations match the table (crash beats everything; a dead marker is promoted; a live one is skipped)", bad == 0);
        CHECK("plan: a .crash is never translated again", n48x_plan(1, 0, 0, 0) == N48X_PL_SKIP_CRASH && n48x_plan(1, 1, 1, 1) == N48X_PL_SKIP_CRASH);
    }

    // ===== T1.6 real marker files =====
    char D[1024]; snprintf(D, sizeof D, "%s/k1", TMP); mkd(D);
    {
        CHECK("marker: create -> 0, again (O_EXCL) -> 1", n48x_marker_create(D, SHA_A) == 0 && n48x_marker_create(D, SHA_A) == 1);
        char p[1100]; n48x_file_path(p, sizeof p, D, SHA_A, N48X_EXT_INFLIGHT);
        CHECK("marker: holds this process id", n48x_marker_pid(p) == (long)getpid());
        struct stat st; stat(p, &st);
        CHECK("marker: mode 0600", (st.st_mode & 0777) == 0600);
        CHECK("probe: a marker of a live process (this one) -> skip, left in place", n48x_probe(D, SHA_A) == N48X_PL_SKIP_LIVE && n48x_exists(p));
        n48x_marker_remove(D, SHA_A);
        CHECK("marker: remove", !n48x_exists(p) && n48x_probe(D, SHA_A) == N48X_PL_TRANSLATE);
        // a dead process's marker
        pid_t c = fork(); if (c == 0) _exit(0);
        int stt = 0; waitpid(c, &stt, 0);
        char pid[32]; snprintf(pid, sizeof pid, "%d\n", (int)c); touch_file(p, pid);
        CHECK("pid_alive: a reaped child is dead, this process is alive, junk is dead", !n48x_pid_alive((long)c) && n48x_pid_alive((long)getpid()) && !n48x_pid_alive(-1) && !n48x_pid_alive(0));
        CHECK("probe: a marker of a DEAD process is promoted to .crash and the sha is skipped", n48x_probe(D, SHA_A) == N48X_PL_PROMOTE_THEN_SKIP && file_exists(D, SHA_A, N48X_EXT_CRASH) && !file_exists(D, SHA_A, N48X_EXT_INFLIGHT));
        CHECK("probe: afterwards the sha stays skipped (the .crash)", n48x_probe(D, SHA_A) == N48X_PL_SKIP_CRASH);
        touch_file(p, "not a number");   // reuse: an unreadable marker counts as dead
        n48x_file_path(p, sizeof p, D, SHA_B, N48X_EXT_INFLIGHT); touch_file(p, "junk\n");
        CHECK("probe: a marker with no pid counts as dead -> promoted", n48x_probe(D, SHA_B) == N48X_PL_PROMOTE_THEN_SKIP && file_exists(D, SHA_B, N48X_EXT_CRASH));
        n48x_file_path(p, sizeof p, D, SHA_C, N48X_EXT_FAIL); touch_file(p, "refused");
        CHECK("probe: a .fail is skipped", n48x_probe(D, SHA_C) == N48X_PL_SKIP_FAIL);
        CHECK("probe: an unusable sha is never translated", n48x_probe(D, "zz") == N48X_PL_SKIP_CRASH);
    }
    {   // the sweep at the first miss
        char sd[1024]; snprintf(sd, sizeof sd, "%s/k2", TMP); mkd(sd);
        pid_t c = fork(); if (c == 0) _exit(0);
        int stt = 0; waitpid(c, &stt, 0);
        char p[1100], pid[32]; snprintf(pid, sizeof pid, "%d\n", (int)c);
        n48x_file_path(p, sizeof p, sd, SHA_A, N48X_EXT_INFLIGHT); touch_file(p, pid);          // dead
        n48x_marker_create(sd, SHA_B);                                                          // live (this process)
        CHECK("sweep: dead markers become .crash, live ones stay, the count is right", n48x_sweep(sd) == 1 && file_exists(sd, SHA_A, N48X_EXT_CRASH) && !file_exists(sd, SHA_A, N48X_EXT_INFLIGHT) && file_exists(sd, SHA_B, N48X_EXT_INFLIGHT));
        n48x_marker_remove(sd, SHA_B);
    }

    // ===== T1.7 trim: the pure decision and the real directory =====
    {
        n48x_ent e[5]; unsigned char del[5];
        memset(e, 0, sizeof e);
        for (int i = 0; i < 5; i++) { snprintf(e[i].sha, 65, "%064d", i); e[i].bytes = 100; e[i].mtime = 1000 + (i == 2 ? -500 : i); }   // entry 2 is the oldest
        CHECK("trim: within the cap nothing goes", n48x_trim_select(e, 5, 500, del) == 0 && !del[0] && !del[1] && !del[2] && !del[3] && !del[4]);
        CHECK("trim: over the cap the OLDEST go first, down to 80% of the cap", n48x_trim_select(e, 5, 450, del) == 2 && del[2] == 1 && del[0] == 1 && !del[1] && !del[3] && !del[4]);   // 500 -> 360 (80% of 450): remove 2 oldest = 200 -> 300
        e[1].bytes = 1000;
        CHECK("trim: one large entry is enough", n48x_trim_select(e, 5, 1000, del) >= 1);
        for (int i = 0; i < 5; i++) { e[i].bytes = 100; e[i].mtime = 7; }
        CHECK("trim: equal times break ties by position (deterministic)", n48x_trim_select(e, 5, 400, del) == 2 && del[0] == 1 && del[1] == 1 && !del[2]);
        CHECK("trim: an empty list is fine", n48x_trim_select(e, 0, 0, del) == 0);
        CHECK("trim: the target is 80% of the cap exactly", ({ n48x_ent t[10]; unsigned char dl[10]; memset(t, 0, sizeof t); for (int i = 0; i < 10; i++) { snprintf(t[i].sha, 65, "%064d", i); t[i].bytes = 10; t[i].mtime = i; } n48x_trim_select(t, 10, 90, dl) == 3; }));   // 100 > 90 -> target 72 -> 3 removed -> 70
    }
    {
        char td[1024]; snprintf(td, sizeof td, "%s/k3", TMP); mkd(td);
        const char *shas[4] = { SHA_A, SHA_B, SHA_C, SHA_D };
        for (int i = 0; i < 4; i++) {
            char p[1100]; char big[1001]; memset(big, 'x', 1000); big[1000] = 0;
            n48x_file_path(p, sizeof p, td, shas[i], N48X_EXT_SPV); touch_file(p, big); set_mtime(p, 1000 + i * 10);
            n48x_file_path(p, sizeof p, td, shas[i], N48X_EXT_META); touch_file(p, big); set_mtime(p, 1000 + i * 10);
        }
        char p[1100]; n48x_file_path(p, sizeof p, td, SHA_A, N48X_EXT_CRASH); touch_file(p, "");   // a marker: never counted, never removed
        const size_t r = n48x_trim_dir(td, 6500);   // 8000 bytes of results > 6500 -> trim to 80% (5200): the 2 oldest shas (4000 bytes) go
        CHECK("trim_dir: removed the two OLDEST shas (all their files) and kept the others", r == 2 && !file_exists(td, SHA_A, N48X_EXT_SPV) && !file_exists(td, SHA_A, N48X_EXT_META) && !file_exists(td, SHA_B, N48X_EXT_SPV) &&
              file_exists(td, SHA_C, N48X_EXT_SPV) && file_exists(td, SHA_D, N48X_EXT_META));
        CHECK("trim_dir: a .crash marker is never removed", file_exists(td, SHA_A, N48X_EXT_CRASH));
        CHECK("trim_dir: within the cap it removes nothing", n48x_trim_dir(td, 1u << 20) == 0 && file_exists(td, SHA_C, N48X_EXT_SPV));
    }

    // ===== T1.8 directory preparation and old keys =====
    {
        char par[1024], k1[1100], k2[1100], other[1100]; snprintf(par, sizeof par, "%s/cache/" N48X_CACHE_SUB, TMP);
        char ucd[1024]; snprintf(ucd, sizeof ucd, "%s/cache", TMP); mkd(ucd);
        snprintf(k1, sizeof k1, "%s/1111111111111111", par); snprintf(k2, sizeof k2, "%s/2222222222222222", par);
        CHECK("prepare: creates parent and key dir (0700)", n48x_prepare_dirs(par, k1, "1111111111111111") == 0 && ({ struct stat st; stat(k1, &st); (st.st_mode & 0777) == 0700; }));
        char f[1200]; snprintf(f, sizeof f, "%s/%s.spv", k1, SHA_A); touch_file(f, "old");
        CHECK("prepare: a new key removes the old key directory with its files", n48x_prepare_dirs(par, k2, "2222222222222222") == 0 && !n48x_exists(k1) && n48x_exists(k2));
        snprintf(f, sizeof f, "%s/%s.spv", k2, SHA_A); touch_file(f, "current");
        CHECK("prepare: the current key's files survive a second prepare", n48x_prepare_dirs(par, k2, "2222222222222222") == 0 && n48x_exists(f));
        // a symlink planted as an old key, pointing at a directory with a file: never followed
        snprintf(other, sizeof other, "%s/elsewhere", TMP); mkd(other);
        char vf[1200]; snprintf(vf, sizeof vf, "%s/precious", other); touch_file(vf, "keep");
        char lnk[1200]; snprintf(lnk, sizeof lnk, "%s/3333333333333333", par);
        CHECK("symlink planted as an old key", symlink(other, lnk) == 0);
        (void)n48x_remove_old_keys(par, "2222222222222222");
        CHECK("remove_old_keys: a symlink is not followed and its target is untouched", n48x_exists(vf));
        unlink(lnk);
        // a symlink planted as the parent is refused
        char par2[1100]; snprintf(par2, sizeof par2, "%s/cache2", TMP); symlink(other, par2);
        CHECK("mkdir_checked: a symlink in place of the directory is refused", n48x_mkdir_checked(par2) != 0);
        unlink(par2);
        CHECK("mkdir_checked: a regular file in place of the directory is refused", ({ char pf[1100]; snprintf(pf, sizeof pf, "%s/afile", TMP); touch_file(pf, "x"); n48x_mkdir_checked(pf) != 0; }));
    }

    // ===== T1.9 the override table =====
    {
        char *txt = slurp(".", ovrPath);
        if (!txt) { char alt[1100]; snprintf(alt, sizeof alt, "%s/%s", dir, ovrPath); txt = slurp(".", alt); }
        CHECK("overrides.txt is readable", txt != NULL);
        size_t found = 0, lines = 0; int allMatch = 1;
        if (txt) {
            for (char *l = strtok(txt, "\n"); l; l = strtok(NULL, "\n")) {
                while (*l == ' ' || *l == '\t') l++;
                if (!*l || *l == '#') continue;
                char *eq = strchr(l, '='); if (!eq) continue;
                *eq = 0; unsigned a, b, c;
                lines++;
                if (sscanf(eq + 1, "%u,%u,%u", &a, &b, &c) != 3) { allMatch = 0; continue; }
                const uint32_t *o = n48x_local_for(l);
                if (!o || o[0] != a || o[1] != b || o[2] != c) allMatch = 0; else found++;
            }
            free(txt);
        }
        CHECK("override table: every line of overrides.txt is in the table with the same size, and the table has no extra", allMatch && found == lines && lines == N48X_NOVERRIDES);
        CHECK("override table: an unlisted kernel gets none", n48x_local_for("some_kernel") == NULL && n48x_local_for(NULL) == NULL && n48x_local_for("") == NULL);
    }

    // ===== T1.10 the Mach-O UUID reader =====
    {
        uint8_t a[16], b[16], mem[16]; char exe[1024]; uint32_t sz = sizeof exe; _NSGetExecutablePath(exe, &sz);
        const int f = n48x_file_uuid(exe, a);
        const int m = n48x_macho_uuid(_dyld_get_image_header(0), 65536, mem);
        CHECK("macho uuid: a thin 64-bit executable has one, and the file's equals the loaded image's", f == 1 && m == 1 && !memcmp(a, mem, 16));
        CHECK("macho uuid: not Mach-O / short -> 0", n48x_macho_uuid("abcdefghijklmnopqrstuvwx", 24, b) == 0 && n48x_macho_uuid(mem, 4, b) == 0 && n48x_file_uuid("/nonexistent/xx", b) == 0);
        uint8_t hdr[256]; memset(hdr, 0, sizeof hdr); struct mach_header_64 *h = (struct mach_header_64 *)hdr; h->magic = MH_MAGIC_64; h->ncmds = 1; h->sizeofcmds = 24;
        struct uuid_command *u = (struct uuid_command *)(hdr + sizeof *h); u->cmd = LC_UUID; u->cmdsize = 24; for (int i = 0; i < 16; i++) u->uuid[i] = (uint8_t)(i + 1);
        CHECK("macho uuid: a synthetic header", n48x_macho_uuid(hdr, sizeof hdr, b) == 1 && b[0] == 1 && b[15] == 16);
        u->cmdsize = 4000;
        CHECK("macho uuid: a load command that runs past the header is refused", n48x_macho_uuid(hdr, sizeof hdr, b) == 0);
    }

    // ===== T1.11 the reader's candidate order =====
    {
        char base[1024]; snprintf(base, sizeof base, "%s/Versions", TMP); mkd(base);
        const char *names[] = { "A", "32023", "Current", "31000", "abc", "5", "99999999999999999999" };
        for (size_t i = 0; i < sizeof names / sizeof *names; i++) { char p[1100]; snprintf(p, sizeof p, "%s/%s", base, names[i]); mkd(p); }
        static char cand[16][N48X_PATH_MAX];
        const size_t n = n48x_reader_candidates(base, cand, 16);
        char want0[1100], want1[1100], want2[1100], want3[1100]; snprintf(want0, sizeof want0, "%s/Current/Libraries/libLLVM.dylib", base); snprintf(want1, sizeof want1, "%s/32023/Libraries/libLLVM.dylib", base);
        snprintf(want2, sizeof want2, "%s/31000/Libraries/libLLVM.dylib", base); snprintf(want3, sizeof want3, "%s/5/Libraries/libLLVM.dylib", base);
        CHECK("reader candidates: Current first, then the numeric version directories, highest first; non-numeric names are ignored", n == 4 && !strcmp(cand[0], want0) && !strcmp(cand[1], want1) && !strcmp(cand[2], want2) && !strcmp(cand[3], want3));
        n48x_reader r; char err[200];
        CHECK("reader: no usable library -> a clean failure, not a crash", n48x_reader_load(&r, base, NULL, err, sizeof err) != 0 && err[0]);
        CHECK("reader: an override path that is not a library -> a clean failure", n48x_reader_load(&r, base, "/nonexistent/libLLVM.dylib", err, sizeof err) != 0);
    }

    // ===== T4 the crash guard, BEFORE any thread exists =====
    {
        char cd[1024]; snprintf(cd, sizeof cd, "%s/k4", TMP); mkd(cd);
        fk_reset(); FK.dir = cd; FK.sha = SHA_A;
        pid_t c = fork();
        if (c == 0) {   // the child: translate with the test-only exit hook set: it dies right after the marker is written
            setenv("N48X_TEST_EXIT_AFTER_MARKER", "1", 1);
            char err[100]; (void)fk_job(cd, SHA_A, "k", err, sizeof err);
            _exit(0);   // never reached when the hook works
        }
        int stt = 0; waitpid(c, &stt, 0);
        CHECK("T4: the child died mid-translation (the test hook's exit code)", WIFEXITED(stt) && WEXITSTATUS(stt) == 77);
        CHECK("T4: it left its marker behind", file_exists(cd, SHA_A, N48X_EXT_INFLIGHT) && !file_exists(cd, SHA_A, N48X_EXT_SPV) && !file_exists(cd, SHA_A, N48X_EXT_META));
        char err[256]; fk_reset();
        int rc = fk_job(cd, SHA_A, "k", err, sizeof err);
        CHECK("T4: the next run skips that sha (E_SKIP_CRASH) and never calls the reader or the translator", rc == N48X_E_SKIP_CRASH && FK.airCalls == 0 && FK.xlCalls == 0);
        CHECK("T4: the dead marker became .crash", file_exists(cd, SHA_A, N48X_EXT_CRASH) && !file_exists(cd, SHA_A, N48X_EXT_INFLIGHT));
        fk_reset(); rc = fk_job(cd, SHA_A, "k", err, sizeof err);
        CHECK("T4: and again on the run after that", rc == N48X_E_SKIP_CRASH && FK.xlCalls == 0);
        fk_reset(); rc = fk_job(cd, SHA_B, "k", err, sizeof err);
        CHECK("T4: other shas still translate", rc == N48X_OK && FK.xlCalls == 1);
        char cd2[1024]; snprintf(cd2, sizeof cd2, "%s/k5", TMP); mkd(cd2);   // a new key is a new directory: the sha is tried again
        fk_reset(); rc = fk_job(cd2, SHA_A, "k", err, sizeof err);
        CHECK("T4: under a new key the sha is retried", rc == N48X_OK && FK.xlCalls == 1);
        // a crash in a child that was killed by a signal rather than a clean exit is the same thing
        char cd3[1024]; snprintf(cd3, sizeof cd3, "%s/k6", TMP); mkd(cd3);
        c = fork();
        if (c == 0) { n48x_marker_create(cd3, SHA_C); raise(SIGKILL); _exit(0); }
        waitpid(c, &stt, 0);
        fk_reset(); rc = fk_job(cd3, SHA_C, "k", err, sizeof err);
        CHECK("T4: a process killed by a signal mid-translation is handled the same way", WIFSIGNALED(stt) && rc == N48X_E_SKIP_CRASH && FK.xlCalls == 0);
    }

    // ===== T1.12 one job's order of operations =====
    {
        char jd[1024]; snprintf(jd, sizeof jd, "%s/j1", TMP); mkd(jd);
        char err[256]; int rc;
        fk_reset(); FK.dir = jd; FK.sha = SHA_A;
        rc = fk_job(jd, SHA_A, "k", err, sizeof err);
        CHECK("job: a good shader is installed (.meta.json and .spv), the marker is gone", rc == N48X_OK && file_exists(jd, SHA_A, N48X_EXT_SPV) && file_exists(jd, SHA_A, N48X_EXT_META) && !file_exists(jd, SHA_A, N48X_EXT_INFLIGHT) && !file_exists(jd, SHA_A, N48X_EXT_FAIL));
        CHECK("job: the crash marker existed while the reader and the translator ran", FK.markerSeenInReader == 1 && FK.markerSeenInXlate == 1);
        CHECK("job: buffers are released (text, spv, meta)", FK.freeCalls == 1 && FK.relCalls == 2);
        char p[1100]; n48x_file_path(p, sizeof p, jd, SHA_A, N48X_EXT_SPV); struct stat st; stat(p, &st);
        CHECK("job: the .spv is world-readable 0644 and not a temporary name", (st.st_mode & 0777) == 0644 && st.st_size == 64);
        // order: meta BEFORE spv
        char jo[1024]; snprintf(jo, sizeof jo, "%s/j2", TMP); mkd(jo);
        n48x_file_path(p, sizeof p, jo, SHA_B, N48X_EXT_SPV); mkd(p);   // block the .spv name with a directory: its rename fails
        fk_reset(); rc = fk_job(jo, SHA_B, "k", err, sizeof err);
        CHECK("job order: with the .spv blocked, the .meta.json was already written (meta first)", rc == N48X_E_IO && file_exists(jo, SHA_B, N48X_EXT_META) && !file_exists(jo, SHA_B, N48X_EXT_INFLIGHT));
        char jm[1024]; snprintf(jm, sizeof jm, "%s/j3", TMP); mkd(jm);
        n48x_file_path(p, sizeof p, jm, SHA_C, N48X_EXT_META); mkd(p);
        fk_reset(); rc = fk_job(jm, SHA_C, "k", err, sizeof err);
        CHECK("job order: with the .meta.json blocked, NO .spv appears (the spv comes last, only after the meta)", rc == N48X_E_IO && !file_exists(jm, SHA_C, N48X_EXT_SPV) && !file_exists(jm, SHA_C, N48X_EXT_INFLIGHT));
        // refusals -> .fail
        char jf[1024]; snprintf(jf, sizeof jf, "%s/j4", TMP); mkd(jf);
        fk_reset(); FK.translateRc = 1;
        rc = fk_job(jf, SHA_A, "k", err, sizeof err);
        CHECK("job: a translator refusal writes .fail, removes the marker, installs nothing", rc == N48X_E_REFUSED && file_exists(jf, SHA_A, N48X_EXT_FAIL) && !file_exists(jf, SHA_A, N48X_EXT_INFLIGHT) && !file_exists(jf, SHA_A, N48X_EXT_SPV) && strstr(err, "fake translator"));
        fk_reset(); rc = fk_job(jf, SHA_A, "k", err, sizeof err);
        CHECK("job: the negative cache: the next run does not call anything", rc == N48X_E_SKIP_FAIL && FK.airCalls == 0 && FK.xlCalls == 0);
        fk_reset(); FK.translateRc = 2; rc = fk_job(jf, SHA_B, "k", err, sizeof err);
        CHECK("job: a caught panic (code 2) is a refusal and is cached too", rc == N48X_E_REFUSED && file_exists(jf, SHA_B, N48X_EXT_FAIL));
        fk_reset(); FK.readerFails = 1; rc = fk_job(jf, SHA_C, "k", err, sizeof err);
        CHECK("job: a reader refusal writes .fail, translator not called", rc == N48X_E_READER && file_exists(jf, SHA_C, N48X_EXT_FAIL) && FK.xlCalls == 0 && !file_exists(jf, SHA_C, N48X_EXT_INFLIGHT));
        fk_reset(); FK.bigText = 1; rc = fk_job(jf, SHA_D, "k", err, sizeof err);
        CHECK("job: LLVM text over the 64 MiB cap is refused before the translator runs", rc == N48X_E_CAP && FK.xlCalls == 0 && file_exists(jf, SHA_D, N48X_EXT_FAIL));
        char jg[1024]; snprintf(jg, sizeof jg, "%s/j5", TMP); mkd(jg);
        fk_reset(); FK.bigSpv = 1; rc = fk_job(jg, SHA_A, "k", err, sizeof err);
        CHECK("job: SPIR-V over the 16 MiB cap is not installed", rc == N48X_E_CAP && !file_exists(jg, SHA_A, N48X_EXT_SPV) && file_exists(jg, SHA_A, N48X_EXT_FAIL));
        fk_reset(); { uint8_t big[8]; rc = n48x_translate_job(&FDEPS, jg, SHA_B, "k", big, N48X_MAX_AIR + 1, err, sizeof err); }
        CHECK("job: bitcode over the 4 MiB cap is refused before anything is written", rc == N48X_E_CAP && FK.airCalls == 0 && !file_exists(jg, SHA_B, N48X_EXT_INFLIGHT) && !file_exists(jg, SHA_B, N48X_EXT_FAIL));
        fk_reset(); rc = n48x_translate_job(&FDEPS, jg, SHA_B, "k", (const uint8_t *)"abc", 3, err, sizeof err);
        CHECK("job: empty / tiny bitcode is refused", rc == N48X_E_CAP && FK.airCalls == 0);
        fk_reset(); rc = fk_job(jg, "short", "k", err, sizeof err);
        CHECK("job: a bad sha is refused", rc == N48X_E_IO && FK.airCalls == 0);
        // a live marker from another process: skipped
        char jl[1024]; snprintf(jl, sizeof jl, "%s/j6", TMP); mkd(jl);
        n48x_marker_create(jl, SHA_A);   // this process: alive
        fk_reset(); rc = fk_job(jl, SHA_A, "k", err, sizeof err);
        CHECK("job: a live process's marker means skip (not our translation to run)", rc == N48X_E_SKIP_LIVE && FK.airCalls == 0 && file_exists(jl, SHA_A, N48X_EXT_INFLIGHT));
        n48x_marker_remove(jl, SHA_A);
        fk_reset(); FK.dir = jl; FK.sha = SHA_B; rc = fk_job(jl, SHA_B, "write_tex", err, sizeof err);
        CHECK("job: a kernel in the override table is still translated (the table is applied inside the library call)", rc == N48X_OK);
    }

    // ===== T1.13 the engine: the worker's stack, timeouts, sharing =====
    {
        char ed[1024]; snprintf(ed, sizeof ed, "%s/e1", TMP); mkd(ed);
        static n48x_engine eng; n48x_deps sd = FDEPS; sd.air_to_text = stack_air;
        n48x_engine_init(&eng, &sd, NULL, NULL, NULL, N48X_WORKER_STACK);
        fk_reset(); char err[256];
        static const uint8_t air[16] = "BC\xc0\xde-----------";
        int rc = n48x_engine_run(&eng, ed, SHA_A, "k", air, sizeof air, 5000, err, sizeof err);
        CHECK("engine: a job runs on the worker and installs its result", rc == N48X_OK && file_exists(ed, SHA_A, N48X_EXT_SPV));
        CHECK("engine: the worker thread has the 64 MiB stack (a default secondary thread has 512 KiB)", FK.stackSeen >= N48X_WORKER_STACK && FK.stackSeen >= (size_t)64u << 20);
        // timeout, then the worker finishes and installs the result
        fk_reset(); FK.delayMs = 400;
        rc = n48x_engine_run(&eng, ed, SHA_B, "k", air, sizeof air, 50, err, sizeof err);
        CHECK("engine: the caller stops waiting after its deadline (E_TIMEOUT)", rc == N48X_E_TIMEOUT && !file_exists(ed, SHA_B, N48X_EXT_SPV));
        for (int i = 0; i < 100 && !file_exists(ed, SHA_B, N48X_EXT_SPV); i++) usleep(50000);
        CHECK("engine: the worker finishes the job after the caller left and installs the result", file_exists(ed, SHA_B, N48X_EXT_SPV) && file_exists(ed, SHA_B, N48X_EXT_META) && !file_exists(ed, SHA_B, N48X_EXT_INFLIGHT));
        // not waiting at all
        fk_reset(); FK.delayMs = 100;
        rc = n48x_engine_run(&eng, ed, SHA_C, "k", air, sizeof air, 0, err, sizeof err);
        CHECK("engine: wait 0 returns at once and the job still completes", rc == N48X_E_TIMEOUT);
        for (int i = 0; i < 100 && !file_exists(ed, SHA_C, N48X_EXT_SPV); i++) usleep(50000);
        CHECK("engine: ... completes in the background", file_exists(ed, SHA_C, N48X_EXT_SPV));
        // sharing: two callers for one sha are one translation
        fk_reset(); FK.delayMs = 300;
        struct share sh = { &eng, ed, -1 };
        pthread_t th; pthread_create(&th, NULL, share_thread, &sh);
        rc = n48x_engine_run(&eng, ed, SHA_D, "k", air, sizeof air, 5000, err, sizeof err);
        pthread_join(th, NULL);
        CHECK("engine: two callers for one sha share one translation", rc == N48X_OK && sh.rc == N48X_OK && FK.xlCalls == 1);
        {   // build 16 (P4): a caller that arrives after the job finished (it missed the running job by a hair) is answered from the installed pair, not by a second translation
            const int before = FK.xlCalls;
            rc = n48x_engine_run(&eng, ed, SHA_D, "k", air, sizeof air, 5000, err, sizeof err);
            CHECK("engine (P4): a repeat request for an already-installed sha returns OK without translating again", rc == N48X_OK && FK.xlCalls == before);
        }
        // unavailable translator
        static n48x_engine bad; n48x_engine_init(&bad, NULL, "/nonexistent-base", "/nonexistent/libLLVM.dylib", "/nonexistent/libn48xlate.dylib", N48X_WORKER_STACK);
        fk_reset(); rc = n48x_engine_run(&bad, ed, SHA_A, "k", air, sizeof air, 5000, err, sizeof err);
        CHECK("engine: libraries that cannot be loaded give E_UNAVAILABLE with a message, not a crash", rc == N48X_E_UNAVAILABLE && err[0]);
        rc = n48x_engine_run(&bad, ed, SHA_B, "k", air, 2, 100, err, sizeof err);
        CHECK("engine: oversize / tiny bitcode is refused before any thread work", rc == N48X_E_CAP);
        // a directory that sweeps on the first job: dead markers become .crash
        char sw[1024]; snprintf(sw, sizeof sw, "%s/e2", TMP); mkd(sw);
        pid_t c = fork(); if (c == 0) _exit(0);
        int stt = 0; waitpid(c, &stt, 0);
        char p[1100], pid[32]; snprintf(pid, sizeof pid, "%d\n", (int)c);
        n48x_file_path(p, sizeof p, sw, SHA_C, N48X_EXT_INFLIGHT); touch_file(p, pid);
        fk_reset(); rc = n48x_engine_run(&eng, sw, SHA_A, "k", air, sizeof air, 5000, err, sizeof err);
        CHECK("engine: the first job in a directory sweeps it (a dead marker of ANOTHER sha becomes .crash)", rc == N48X_OK && file_exists(sw, SHA_C, N48X_EXT_CRASH) && !file_exists(sw, SHA_C, N48X_EXT_INFLIGHT));
    }
    {   // spawn: a thread made by n48x_spawn gets exactly the stack asked for (>= 64 MiB); one made the default way does not
        seen_stack = 0;
        pthread_t t; n48x_spawn(&t, probe_stack, NULL, N48X_WORKER_STACK);
        for (int i = 0; i < 100 && !seen_stack; i++) usleep(10000);
        CHECK("spawn: n48x_spawn(..., N48X_WORKER_STACK) gives a 64 MiB stack", seen_stack >= N48X_WORKER_STACK);
        pthread_attr_t a; size_t def = 0; pthread_attr_init(&a); pthread_attr_getstacksize(&a, &def); pthread_attr_destroy(&a);
        CHECK("spawn: the default secondary-thread stack is far smaller (this is what the 64 MiB avoids)", def < (size_t)8u << 20);
    }

    // ===== G source pins on Navi48Device.m =====
    {
        char *src = slurp(dir, "Navi48Device.m");
        CHECK("Navi48Device.m is readable", src != NULL);
        if (src) {
            const char *act = fn_start(src, "static BOOL n48x_active(void) {");
            const char *actEnd = act ? strstr(act, "\n}\n") : NULL;
            CHECK("G: n48x_active calls n48x_applies with WindowServer first, then admitted, the root-allow tool, N48M_TEST_INPROC, the kill file and the environment",
                  has(act, actEnd, "n48x_applies(n48_is_ws(), atomic_load(&n48_app_admitted), n48_allow(), n48x_test_inproc(), N48XL.killed, N48XL.envDirty)"));
            const char *gr = fn_start(src, "static void n48x_gate_read(void) {");
            const char *grEnd = gr ? strstr(gr, "\n}\n") : NULL;
            CHECK("G: the kill file is read with n48x_killed_from_stat (fail closed) and the environment with n48x_env_dirty, never in WindowServer",
                  has(gr, grEnd, "if (!n48_is_ws())") && has(gr, grEnd, "stat(N48X_KILL_FILE, &st)") && has(gr, grEnd, "N48XL.killed = n48x_killed_from_stat(rc, er);") && has(gr, grEnd, "N48XL.envDirty = n48x_env_dirty(*_NSGetEnviron());"));
            const char *dirs = fn_start(src, "static NSArray<NSString *> *n48_spv_dirs(void) {");
            const char *dirsEnd = dirs ? strstr(dirs, "\n}\n") : NULL;
            const char *pb = dirs ? strstr(dirs, "NSMutableArray *m = [bundleOnly mutableCopy];") : NULL;
            const char *pu = dirs ? strstr(dirs, "if (u) { [m addObject:u]; userFor = u; }") : NULL;
            const char *ps = dirs ? strstr(dirs, "if (!skipSide) [m addObject:n48_side_dir()];") : NULL;
            CHECK("G: lookup order: the bundle spvcache first, then the user cache directory, then the side directory", before(pb, pu) && before(pu, ps) && pb < dirsEnd && ps < dirsEnd);
            CHECK("G: the bundle spvcache is the first entry of the array the user directory is appended to", has(dirs, dirsEnd, "bundleOnly = @[ [[[NSBundle bundleForClass:[Navi48Device class]] resourcePath] stringByAppendingPathComponent:@\"spvcache\"] ];"));
            CHECK("G: the side directory is skipped for a sandboxed non-WindowServer process that translates in process",
                  has(dirs, dirsEnd, "const BOOL skipSide = u && n48x_skip_side_dir(n48_is_ws(), n48x_sandboxed());") && has(src, NULL, "N48XL.sandboxed = sandbox_check(getpid(), NULL, 0) != 0;"));
            const char *su = fn_start(src, "static void n48x_setup_locked(void) {");
            const char *suEnd = su ? strstr(su, "\n}\n") : NULL;
            CHECK("G: the key is made from the translator's UUID, the reader's identity, the override hash and kern.osversion",
                  has(su, suEnd, "n48x_file_uuid(n48x_xlib_path().fileSystemRepresentation, xl)") && has(su, suEnd, "n48x_override_hash(ov)") && has(su, suEnd, "n48x_key(key, xl, rd, ov, osv)") && has(su, suEnd, "_dyld_get_shared_cache_uuid(cu)") &&
                  has(su, suEnd, "sysctlbyname(\"kern.osversion\""));
            CHECK("G: the user cache directory is set up by n48x_user_dir after admission, not inside n48_spv_dirs' dispatch_once", has(src, NULL, "static NSString *n48x_user_dir(void) {\n    if (!n48x_active()) return nil;") && !has(dirs, strstr(dirs, "dispatch_once(&o, ^{"), "n48x_user_dir"));
            const char *tf = fn_start(src, "static BOOL n48x_translate_fn(id fn, const char *role, uint64_t deadlineNs) {");
            const char *tfEnd = tf ? strstr(tf, "\n}\n") : NULL;
            CHECK("G: the engine's worker gets N48X_WORKER_STACK (64 MiB)", has(tf, tfEnd, "n48x_engine_init(&N48XL.eng, NULL, NULL, rl, n48x_xlib_path().fileSystemRepresentation, N48X_WORKER_STACK);"));
            CHECK("G: the wait obeys the 3-strike rule of n48_gate.h", has(tf, tfEnd, "n48g_sync_allowed(&N48XL.sw)") && has(tf, tfEnd, "n48g_sync_result(&N48XL.sw, 1)") && has(tf, tfEnd, "n48g_sync_result(&N48XL.sw, 0)") && has(tf, tfEnd, "N48G_SYNC_WAIT_MS"));
            // the three miss branches: translate in process first, the dump and the daemon wait only when not
            const char *ri = fn_start(src, "- (instancetype)initWithDevice:(id)dev descriptor:(MTLRenderPipelineDescriptor *)d noFallback:(BOOL)nofb error:(NSError **)err {");
            const char *riEnd = ri ? strstr(ri, "\n}\n") : NULL;
            const char *r1 = ri ? strstr(ri, "n48x_translate_fn(d.vertexFunction, \"vertex\", dl)") : NULL;
            const char *r2 = ri ? strstr(ri, "(void)n48_dump_pipeline(d);") : NULL;
            const char *r3 = ri ? strstr(ri, "n48_sync_wait(\"render\"") : NULL;
            CHECK("G: render miss: the in-process translation runs before the AIR dump and the daemon wait, which are skipped when it applies",
                  before(r1, r2) && before(r2, r3) && r3 < riEnd && has(ri, riEnd, "if (!inproc) (void)n48_dump_pipeline(d);") && has(ri, riEnd, "if (!inproc && n48g_syncwait_applies(") && has(ri, riEnd, "(e.code == 41 && !nofb && n48x_active())"));
            const char *np = fn_start(src, "- (id)n48NewComputePipeline:(id)fn error:(NSError **)error {");
            const char *npEnd = np ? strstr(np, "\n}\n") : NULL;
            const char *c1 = np ? strstr(np, "n48x_translate_fn(fn, \"kernel\"") : NULL;
            const char *c2 = np ? strstr(np, "n48_sync_wait(\"kernel\"") : NULL;
            CHECK("G: compute miss: in-process first, then (only if not in process) the dump + wait; the placeholder stays the last resort",
                  before(c1, c2) && c2 < npEnd && has(np, npEnd, "if (!inproc && e.code == 41 && n48_fallback_ok() && n48g_syncwait_applies(") && has(np, npEnd, "initPlaceholderWithDevice:self function:fn"));
            const char *nn = fn_start(src, "- (id)n48NewPipeline:(MTLRenderPipelineDescriptor *)d error:(NSError **)error {");
            const char *nnEnd = nn ? strstr(nn, "\n}\n") : NULL;
            CHECK("G: n48NewPipeline logs LINKAGE IGNORED first and does not dump after an in-process attempt", has(nn, nnEnd, "n48x_linkage_render(d);") && has(nn, nnEnd, "NSError *dump = n48x_active() ? e : n48_dump_pipeline(d);"));
            CHECK("G: LINKAGE IGNORED is logged by all four compute descriptor methods", ({ int n = 0; const char *q = src; while ((q = strstr(q, "n48x_linkage_compute(d); N48LOG(\"newComputePipelineStateWithDescriptor:"))) { n++; q++; } n == 4; }));
            CHECK("G: the LINKAGE IGNORED line exists", has(src, NULL, "N48LOG(\"LINKAGE IGNORED %s '%s': "));
            CHECK("G: a function with no bitcodeData is logged once per process (CI / stitched instrumentation)", has(src, NULL, "N48_ONCE(\"function '%s' (class %s) has no bitcodeData"));
            CHECK("G: a user-cache hit is touched (least-recently-used)", has(src, NULL, "n48x_touch_hit(path, sha);"));
            free(src);
        }
        char *hdr = slurp(dir, "n48_xlate.h");
        CHECK("G: n48_xlate.h is readable", hdr != NULL);
        if (hdr) {
            const char *tj = fn_start(hdr, "static inline int n48x_translate_job_x(");
            const char *tjEnd = tj ? strstr(tj, "\n}\n") : NULL;
            const char *m1 = tj ? strstr(tj, "n48x_probe(dir, sha)") : NULL, *m2 = tj ? strstr(tj, "n48x_marker_create(dir, sha)") : NULL, *m3 = tj ? strstr(tj, "dp->air_to_text(") : NULL, *m4 = tj ? strstr(tj, "dp->translate(") : NULL;
            const char *w1 = tj ? strstr(tj, "N48X_EXT_META) || n48x_write_atomic") : NULL, *w2 = tj ? strstr(tj, "N48X_EXT_SPV) || n48x_write_atomic") : NULL, *rm = tj ? strstr(tj, "n48x_marker_remove(dir, sha);") : NULL;
            CHECK("G: job order in the header: probe, marker, reader, translator, meta, spv, marker removed", before(m1, m2) && before(m2, m3) && before(m3, m4) && before(m4, w1) && before(w1, w2) && before(w2, rm) && rm < tjEnd);
            const char *rp = fn_start(hdr, "static inline int n48x_reader_print(");
            const char *rpEnd = rp ? strstr(rp, "\n}\n") : NULL;
            CHECK("G: the reader switches Apple's LLVM context to opaque pointers BEFORE it reads (else typed-pointer text the translator refuses)",
                  has(rp, rpEnd, "if (r->SetOpaquePointers) r->SetOpaquePointers(ctx, 1);") && before(strstr(rp, "SetOpaquePointers(ctx, 1)"), strstr(rp, "ParseBitcodeInContext2(")) && has(hdr, NULL, "_ZNK4llvm11LLVMContext17setOpaquePointersEb"));
            free(hdr);
        }
    }

    printf("test-xlate: %d checks, %d failed\n", runs, fails);
    return fails ? 1 : 0;
}
