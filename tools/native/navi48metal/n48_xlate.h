// n48_xlate.h - bundle 14: in-process shader translation for sandboxed applications (an internal design note, sections 1.1-1.7).
// Pure C: no Metal, no Foundation, no IOKit. Host test test-xlate.c compiles this very header and drives the very functions Navi48Device.m calls; test-xlate-corpus.sh drives the real
// reader and translator through the real engine over the shader corpus.
//
// What it does. A pipeline whose SPIR-V is in neither the bundle's spvcache nor the cache directories is, in an ADMITTED APPLICATION (or an N48M_ALLOW root tool with N48M_TEST_INPROC=1), translated INSIDE
// the process: the AIR bitcode is turned into LLVM text by the operating system's own LLVM (GPUCompiler.framework's libLLVM, loaded with dlopen on the first miss only, ten functions), the text goes to
// Resources/libn48xlate.dylib (the LGPL translator, a C ABI), and the SPIR-V plus its trimmed meta are installed in the user's cache directory. WindowServer never applies: it keeps the precompiled
// spvcache plus the daemon. Nothing here starts a process, and nothing is written outside the cache directory.
//
// Safety shape (section 1.6/1.7): a gate (n48x_applies), a crash marker per translation (<sha>.inflight, O_EXCL, holding the pid; a leftover marker whose process is dead becomes <sha>.crash and that sha is
// never translated again under this key), a negative cache (<sha>.fail), size caps, one worker thread on a 64 MiB stack, jobs one at a time, and every failure falling back to today's behaviour.
#ifndef N48_XLATE_H
#define N48_XLATE_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <dlfcn.h>
#include <pthread.h>
#include <signal.h>
#include <time.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <mach-o/loader.h>
#include <mach-o/dyld.h>
#include <CommonCrypto/CommonDigest.h>
#include "n48_gate.h"

// ---- constants ----
#define N48X_KEY_TAG       "n48x-1"
#define N48X_ABI_VERSION   2u   // build 18 (P3): 2 = n48x_translate_linked added (n48x_translate unchanged)
#define N48X_MAX_DEPS      64u   // linked dependencies one pipeline stage may carry (more = no linked translation)
#define N48X_MAX_DEP_AIR   (8u * 1024u * 1024u)   // total bitcode of all dependencies of one stage
#define N48X_MAX_AIR       (4u * 1024u * 1024u)        // bitcode: the daemon's cap
#define N48X_MAX_LL        (64u * 1024u * 1024u)       // LLVM text
#define N48X_MAX_SPV       (16u * 1024u * 1024u)       // SPIR-V
#define N48X_MAX_META      (8u * 1024u * 1024u)        // trimmed meta JSON
#define N48X_CACHE_CAP     (256ull * 1024ull * 1024ull)   // per application; trimmed to 80% when exceeded
#define N48X_WORKER_STACK  ((size_t)64u * 1024u * 1024u)   // a default secondary thread has 512 KiB; the command-line tool's 8 MiB main stack is the only depth anyone has measured
#define N48X_KILL_FILE     "/private/tmp/n48m-noinproc"
#define N48X_CACHE_SUB     "com.navi48.xlate"
#define N48X_ENV_PREFIX    "METAL2VULKAN_"
#define N48X_LIB_NAME      "libn48xlate.dylib"
#define N48X_GPUC_BASE     "/System/Library/PrivateFrameworks/GPUCompiler.framework/Versions"
#define N48X_PATH_MAX      1024

// ---- result codes of one job ----
#define N48X_OK            0
#define N48X_E_CAP         1    // bitcode empty / too large, LLVM text / SPIR-V / meta over its cap
#define N48X_E_SKIP_CRASH  2    // <sha>.crash exists (or a dead process left <sha>.inflight): never translated again under this key
#define N48X_E_SKIP_FAIL   3    // <sha>.fail exists: refused before, retried only when the key changes
#define N48X_E_SKIP_LIVE   4    // <sha>.inflight belongs to a LIVE process (another instance is translating it right now)
#define N48X_E_READER      5    // the bitcode reader refused the module
#define N48X_E_REFUSED     6    // the translator refused (code 1) or panicked (code 2) or rejected its arguments (code 3)
#define N48X_E_IO          7    // could not create the marker / write the result
#define N48X_E_UNAVAILABLE 8    // the reader or the translator library could not be loaded
#define N48X_E_TIMEOUT     9    // the caller stopped waiting; the worker finishes and installs the result

// ======================================================================================================================================================================
// 1. The gate (section 1.6)
// ======================================================================================================================================================================
// isWs        WindowServer: NEVER (it keeps the precompiled spvcache plus the daemon and never loads either library).
// admitted    the kernel admitted this (class OTHER) process as an allow-listed application.
// rootAllow   an N48M_ALLOW=1 root tool; it applies only with N48M_TEST_INPROC=1 (this retires the add-air.py seeding root probes needed).
// killed      /private/tmp/n48m-noinproc exists or could not be read (fail closed), read once per process.
// envDirty    a METAL2VULKAN_* variable is present: several change the translator's output (RELOOPER_MAX_BLOCKS, WHOLE_PART, TIR_ONLY, ...), and the cache is keyed without them.
static inline int n48x_applies(int isWs, int admitted, int rootAllow, int testInproc, int killed, int envDirty) {
    if (isWs) return 0;
    if (killed) return 0;
    if (envDirty) return 0;
    if (rootAllow) return testInproc ? 1 : 0;
    return admitted ? 1 : 0;
}
// 1 when any variable in envp (NULL-terminated, like environ) starts with METAL2VULKAN_.
static inline int n48x_env_dirty(char *const *envp) {
    if (!envp) return 0;
    for (; *envp; envp++) if (strncmp(*envp, N48X_ENV_PREFIX, sizeof N48X_ENV_PREFIX - 1) == 0) return 1;
    return 0;
}
// The kill file, with n48_gate.h's states: anything but a clean "absent" is killed (fail closed).
static inline int n48x_killed_from_stat(int statRc, int err) { return n48g_kill_state(statRc, err) != N48G_KILL_ABSENT; }
// The side directory /private/var/tmp/n48m-spv is read by WindowServer's sandbox profile and by root tools; a sandboxed application is denied it (deny-log spam), so it is skipped there.
static inline int n48x_skip_side_dir(int isWs, int sandboxed) { return !isWs && sandboxed; }

// ======================================================================================================================================================================
// 2. The --local override table (matches autotranslate/overrides.txt; test-xlate.c compares them)
// ======================================================================================================================================================================
typedef struct { const char *name; uint32_t xyz[3]; } n48x_override;
static const n48x_override n48x_overrides[] = {
    { "sum_rgba_columns", { 32, 1, 1 } },
    { "sum_rgba_rows",    { 32, 1, 1 } },
    { "write_tex",        {  8, 8, 1 } },
};
#define N48X_NOVERRIDES (sizeof n48x_overrides / sizeof n48x_overrides[0])
// The nominal threadgroup size for the kernel named `name`, or NULL (the translator's default 64,1,1).
static inline const uint32_t *n48x_local_for(const char *name) {
    if (!name) return NULL;
    for (size_t i = 0; i < N48X_NOVERRIDES; i++) if (strcmp(n48x_overrides[i].name, name) == 0) return n48x_overrides[i].xyz;
    return NULL;
}

// ======================================================================================================================================================================
// 3. The cache key and the paths (section 1.4)
// ======================================================================================================================================================================
static inline void n48x_hex(char *out, const uint8_t *p, size_t n) {
    static const char d[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) { out[2 * i] = d[p[i] >> 4]; out[2 * i + 1] = d[p[i] & 15]; }
    out[2 * n] = 0;
}
// sha256 over the override table as the lines "name=x,y,z\n" in table order: a change to the table is a new key.
static inline void n48x_override_hash(uint8_t out[CC_SHA256_DIGEST_LENGTH]) {
    CC_SHA256_CTX c; CC_SHA256_Init(&c);
    for (size_t i = 0; i < N48X_NOVERRIDES; i++) {
        char l[160]; const int n = snprintf(l, sizeof l, "%s=%u,%u,%u\n", n48x_overrides[i].name, n48x_overrides[i].xyz[0], n48x_overrides[i].xyz[1], n48x_overrides[i].xyz[2]);
        CC_SHA256_Update(&c, l, (CC_LONG)n);
    }
    CC_SHA256_Final(out, &c);
}
// K: the first 16 hex digits of sha256 over: "n48x-1", the build UUID of libn48xlate, the identity of the reader library, the hash of the override table, kern.osversion.
// readerId is the reader's LC_UUID when it is known from a loaded image, or (a reader in the dyld shared cache has no file and is not loaded at lookup time) the shared cache's UUID, which pins
// every library in it, GPUCompiler's included. out needs 17 bytes.
static inline void n48x_key(char out[17], const uint8_t xlUuid[16], const uint8_t readerId[16], const uint8_t ovHash[32], const char *osversion) {
    CC_SHA256_CTX c; CC_SHA256_Init(&c);
    CC_SHA256_Update(&c, N48X_KEY_TAG, (CC_LONG)(sizeof N48X_KEY_TAG));   // includes the NUL: a fixed delimiter
    CC_SHA256_Update(&c, xlUuid, 16);
    CC_SHA256_Update(&c, readerId, 16);
    CC_SHA256_Update(&c, ovHash, 32);
    const char *os = osversion ? osversion : "";
    CC_SHA256_Update(&c, os, (CC_LONG)strlen(os));
    uint8_t d[CC_SHA256_DIGEST_LENGTH]; CC_SHA256_Final(d, &c);
    n48x_hex(out, d, 8);
}
static inline int n48x_sha_ok(const char *s) {
    if (!s) return 0;
    for (int i = 0; i < 64; i++) { const char c = s[i]; if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return 0; }
    return s[64] == 0;
}
static inline int n48x_key_ok(const char *k) {
    if (!k) return 0;
    for (int i = 0; i < 16; i++) { const char c = k[i]; if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return 0; }
    return k[16] == 0;
}
// ---- build 18 (P3): the cache key of a LINKED translation ----
// The same entry function linked to different functions is a different program (the dependency decides the output: n48xlate/tests/linked.rs), so the key is
//   sha256( "n48x-link-1\n" + entrySha(64 hex) + "\n" + for each dependency, sorted by (symbol, sha) with exact duplicates dropped: symbol + "\0" + sha(64 hex) + "\n" ).
// The order the client listed the functions in, and a function listed twice, do not change the key; a different function, a different name, an added or a removed dependency do. 0 = ok, else out is empty.
typedef struct { const char *sym; const char *sha; } n48x_lkpair;
static inline int n48x_link_cmp(const void *a, const void *b) {
    const n48x_lkpair *x = (const n48x_lkpair *)a, *y = (const n48x_lkpair *)b;
    const int c = strcmp(x->sym, y->sym);
    return c ? c : strcmp(x->sha, y->sha);
}
static inline int n48x_link_sha(char out[65], const char *entrySha, size_t n, const char *const *syms, const char *const *shas) {
    out[0] = 0;
    if (!n48x_sha_ok(entrySha) || n > 4096) return -1;
    n48x_lkpair *v = (n48x_lkpair *)calloc(n ? n : 1, sizeof *v);
    if (!v) return -1;
    for (size_t i = 0; i < n; i++) { if (!syms[i] || !syms[i][0] || !n48x_sha_ok(shas[i])) { free(v); return -1; } v[i].sym = syms[i]; v[i].sha = shas[i]; }
    qsort(v, n, sizeof *v, n48x_link_cmp);
    CC_SHA256_CTX c; CC_SHA256_Init(&c);
    CC_SHA256_Update(&c, "n48x-link-1\n", 12);
    CC_SHA256_Update(&c, entrySha, 64); CC_SHA256_Update(&c, "\n", 1);
    for (size_t i = 0; i < n; i++) {
        if (i && !n48x_link_cmp(&v[i], &v[i - 1])) continue;
        CC_SHA256_Update(&c, v[i].sym, (CC_LONG)strlen(v[i].sym)); CC_SHA256_Update(&c, "\0", 1); CC_SHA256_Update(&c, v[i].sha, 64); CC_SHA256_Update(&c, "\n", 1);
    }
    uint8_t d[CC_SHA256_DIGEST_LENGTH]; CC_SHA256_Final(d, &c);
    n48x_hex(out, d, 32);
    free(v);
    return 0;
}
// <userCacheDir>/com.navi48.xlate  (userCacheDir is confstr(_CS_DARWIN_USER_CACHE_DIR), which ends with '/')
static inline size_t n48x_parent_path(char *out, size_t n, const char *userCacheDir) {
    if (!out || n == 0) return 0;
    out[0] = 0;
    if (!userCacheDir || !userCacheDir[0]) return 0;
    const size_t dl = strlen(userCacheDir);
    const int w = snprintf(out, n, "%s%s" N48X_CACHE_SUB, userCacheDir, userCacheDir[dl - 1] == '/' ? "" : "/");
    if (w <= 0 || (size_t)w >= n) { out[0] = 0; return 0; }
    return (size_t)w;
}
// <userCacheDir>/com.navi48.xlate/<K>
static inline size_t n48x_dir_path(char *out, size_t n, const char *userCacheDir, const char *key) {
    if (!out || n == 0) return 0;
    out[0] = 0;
    if (!n48x_key_ok(key)) return 0;
    char p[N48X_PATH_MAX];
    if (!n48x_parent_path(p, sizeof p, userCacheDir)) return 0;
    const int w = snprintf(out, n, "%s/%s", p, key);
    if (w <= 0 || (size_t)w >= n) { out[0] = 0; return 0; }
    return (size_t)w;
}
// <dir>/<sha><ext>  with ext one of .spv .meta.json .fail .inflight .crash
#define N48X_EXT_SPV      ".spv"
#define N48X_EXT_META     ".meta.json"
#define N48X_EXT_FAIL     ".fail"
#define N48X_EXT_INFLIGHT ".inflight"
#define N48X_EXT_CRASH    ".crash"
static inline size_t n48x_file_path(char *out, size_t n, const char *dir, const char *sha, const char *ext) {
    if (!out || n == 0) return 0;
    out[0] = 0;
    if (!dir || !n48x_sha_ok(sha) || !ext) return 0;
    const int w = snprintf(out, n, "%s/%s%s", dir, sha, ext);
    if (w <= 0 || (size_t)w >= n) { out[0] = 0; return 0; }
    return (size_t)w;
}

// ---- the cache directory: created 0700, lstat-checked (a real directory owned by the caller, never a symlink), old keys removed (section 1.4) ----
// mkdir (EEXIST is fine), then lstat: only a real directory owned by euid passes. 0 = ok.
static inline int n48x_mkdir_checked(const char *path) {
    if (mkdir(path, 0700) != 0 && errno != EEXIST) return -1;
    struct stat st;
    if (lstat(path, &st) != 0 || !S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode) || st.st_uid != geteuid()) return -1;
    return 0;
}
// Removes every key directory under parent except `keep`: plain files (lstat) are unlinked, then rmdir. A symlink, or anything that is not a real directory of ours, is left alone and never followed.
// Returns the number of directories removed.
static inline int n48x_remove_old_keys(const char *parent, const char *keep) {
    DIR *d = opendir(parent);
    if (!d) return 0;
    int removed = 0; struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        const char *nm = e->d_name;
        if (!strcmp(nm, ".") || !strcmp(nm, "..") || !strcmp(nm, keep)) continue;
        char sub[N48X_PATH_MAX]; struct stat st;
        if (snprintf(sub, sizeof sub, "%s/%s", parent, nm) >= (int)sizeof sub) continue;
        if (lstat(sub, &st) != 0 || !S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode) || st.st_uid != geteuid()) continue;
        DIR *sd = opendir(sub);
        if (!sd) continue;
        struct dirent *f;
        while ((f = readdir(sd)) != NULL) {
            if (!strcmp(f->d_name, ".") || !strcmp(f->d_name, "..")) continue;
            char fp[N48X_PATH_MAX + 300]; struct stat fs;
            if (snprintf(fp, sizeof fp, "%s/%s", sub, f->d_name) >= (int)sizeof fp) continue;
            if (lstat(fp, &fs) == 0 && S_ISREG(fs.st_mode)) (void)unlink(fp);
        }
        closedir(sd);
        if (rmdir(sub) == 0) removed++;
    }
    closedir(d);
    return removed;
}
// parent + parent/<key> exist and pass the checks, and old keys are gone. 0 = ready.
static inline int n48x_prepare_dirs(const char *parent, const char *dir, const char *key) {
    if (n48x_mkdir_checked(parent) != 0) return -1;
    if (n48x_mkdir_checked(dir) != 0) return -1;
    (void)n48x_remove_old_keys(parent, key);
    return 0;
}

// ======================================================================================================================================================================
// 4. Size caps (section 1.5)
// ======================================================================================================================================================================
static inline int n48x_air_size_ok(size_t n) { return n >= 4 && n <= N48X_MAX_AIR; }
static inline int n48x_ll_size_ok(size_t n)  { return n >= 1 && n <= N48X_MAX_LL; }
static inline int n48x_spv_size_ok(size_t n) { return n >= 20 && (n & 3u) == 0 && n <= N48X_MAX_SPV; }
static inline int n48x_meta_size_ok(size_t n) { return n >= 2 && n <= N48X_MAX_META; }

// ======================================================================================================================================================================
// 5. The crash-marker state machine (section 1.7)
// ======================================================================================================================================================================
#define N48X_PL_TRANSLATE          0   // nothing is known against this sha: write the marker and translate
#define N48X_PL_SKIP_CRASH         1   // <sha>.crash: a translation of this sha killed a process before; never again under this key
#define N48X_PL_SKIP_FAIL          2   // <sha>.fail: the translator refused it before
#define N48X_PL_SKIP_LIVE          3   // <sha>.inflight of a live process
#define N48X_PL_PROMOTE_THEN_SKIP  4   // <sha>.inflight of a DEAD process: it becomes <sha>.crash, and the sha is skipped
static inline int n48x_plan(int hasCrash, int hasFail, int hasInflight, int inflightAlive) {
    if (hasCrash) return N48X_PL_SKIP_CRASH;
    if (hasInflight) return inflightAlive ? N48X_PL_SKIP_LIVE : N48X_PL_PROMOTE_THEN_SKIP;
    if (hasFail) return N48X_PL_SKIP_FAIL;
    return N48X_PL_TRANSLATE;
}
static inline int n48x_pid_alive(long pid) {
    if (pid <= 0) return 0;
    if (kill((pid_t)pid, 0) == 0) return 1;
    return errno == EPERM;   // exists, not ours
}
static inline int n48x_exists(const char *path) { struct stat st; return lstat(path, &st) == 0; }
// Reads the pid out of a marker; -1 when the file is unreadable or holds no number (a marker nobody can vouch for counts as DEAD: the translation that wrote it cannot be known to be running).
static inline long n48x_marker_pid(const char *path) {
    char b[32]; const int fd = open(path, O_RDONLY | O_NOFOLLOW);
    if (fd < 0) return -1;
    const ssize_t n = read(fd, b, sizeof b - 1);
    close(fd);
    if (n <= 0) return -1;
    b[n] = 0;
    char *e = NULL; const long v = strtol(b, &e, 10);
    return (e && e != b) ? v : -1;
}
// One sha's state in dir -> plan. Promotes a dead marker to .crash (rename, no link followed: both names are ours in a directory we own).
static inline int n48x_probe(const char *dir, const char *sha) {
    char crash[N48X_PATH_MAX], fail[N48X_PATH_MAX], infl[N48X_PATH_MAX];
    if (!n48x_file_path(crash, sizeof crash, dir, sha, N48X_EXT_CRASH) || !n48x_file_path(fail, sizeof fail, dir, sha, N48X_EXT_FAIL) || !n48x_file_path(infl, sizeof infl, dir, sha, N48X_EXT_INFLIGHT)) return N48X_PL_SKIP_CRASH;   // unusable names: never translate
    const int hasInfl = n48x_exists(infl);
    const int alive = hasInfl ? n48x_pid_alive(n48x_marker_pid(infl)) : 0;
    const int plan = n48x_plan(n48x_exists(crash), n48x_exists(fail), hasInfl, alive);
    if (plan == N48X_PL_PROMOTE_THEN_SKIP) { if (rename(infl, crash) != 0) (void)unlink(infl); }
    return plan;
}
// Creates <sha>.inflight with O_EXCL (mode 0600) holding the pid. 0 = created, 1 = already exists, -1 = error.
static inline int n48x_marker_create(const char *dir, const char *sha) {
    char infl[N48X_PATH_MAX];
    if (!n48x_file_path(infl, sizeof infl, dir, sha, N48X_EXT_INFLIGHT)) return -1;
    const int fd = open(infl, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    if (fd < 0) return errno == EEXIST ? 1 : -1;
    char b[32]; const int n = snprintf(b, sizeof b, "%d\n", (int)getpid());
    const ssize_t w = write(fd, b, (size_t)n);
    const int c = close(fd);
    if (w != n || c != 0) { (void)unlink(infl); return -1; }
    return 0;
}
static inline void n48x_marker_remove(const char *dir, const char *sha) {
    char infl[N48X_PATH_MAX];
    if (n48x_file_path(infl, sizeof infl, dir, sha, N48X_EXT_INFLIGHT)) (void)unlink(infl);
}
// The first miss in a process: every leftover marker in dir whose process is dead becomes <sha>.crash. Returns how many were promoted.
static inline int n48x_sweep(const char *dir) {
    DIR *d = opendir(dir);
    if (!d) return 0;
    int n = 0; struct dirent *e;
    const size_t el = sizeof N48X_EXT_INFLIGHT - 1;
    while ((e = readdir(d)) != NULL) {
        const size_t l = strlen(e->d_name);
        if (l != 64 + el || strcmp(e->d_name + 64, N48X_EXT_INFLIGHT) != 0) continue;
        char sha[65]; memcpy(sha, e->d_name, 64); sha[64] = 0;
        if (!n48x_sha_ok(sha)) continue;
        if (n48x_probe(dir, sha) == N48X_PL_PROMOTE_THEN_SKIP) n++;
    }
    closedir(d);
    return n;
}

// ======================================================================================================================================================================
// 6. The size cap and the least-recently-used trim (section 1.4)
// ======================================================================================================================================================================
typedef struct { char sha[65]; int64_t mtime; uint64_t bytes; } n48x_ent;   // one entry per sha: its newest modification time and the bytes of its .meta.json + .spv + .fail
typedef struct { int64_t mtime; size_t idx; } n48x_ord;
static inline int n48x_ord_cmp(const void *a, const void *b) {
    const n48x_ord *x = (const n48x_ord *)a, *y = (const n48x_ord *)b;
    if (x->mtime != y->mtime) return x->mtime < y->mtime ? -1 : 1;
    return x->idx < y->idx ? -1 : (x->idx > y->idx ? 1 : 0);
}
// Fills del[i] = 1 for the entries to remove: nothing while the total is within capBytes, else the oldest entries until the total is at or below 80% of the cap. Returns how many.
static inline size_t n48x_trim_select(const n48x_ent *e, size_t n, uint64_t capBytes, unsigned char *del) {
    uint64_t total = 0;
    for (size_t i = 0; i < n; i++) { del[i] = 0; total += e[i].bytes; }
    if (total <= capBytes) return 0;
    const uint64_t target = capBytes / 10u * 8u;
    n48x_ord *o = (n48x_ord *)malloc((n ? n : 1) * sizeof *o);
    if (!o) return 0;
    for (size_t i = 0; i < n; i++) { o[i].mtime = e[i].mtime; o[i].idx = i; }
    qsort(o, n, sizeof *o, n48x_ord_cmp);
    size_t k = 0;
    for (size_t j = 0; j < n && total > target; j++) { del[o[j].idx] = 1; total -= e[o[j].idx].bytes; k++; }
    free(o);
    return k;
}
// Executes the trim on one cache directory. Plain files only (lstat); .crash / .inflight markers are never counted and never removed. Returns the number of shas removed.
static inline size_t n48x_trim_dir(const char *dir, uint64_t capBytes) {
    DIR *d = opendir(dir);
    if (!d) return 0;
    size_t cap = 256, n = 0; n48x_ent *e = (n48x_ent *)malloc(cap * sizeof *e);
    if (!e) { closedir(d); return 0; }
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        const char *nm = de->d_name; const size_t l = strlen(nm);
        if (l < 65 || nm[64] != '.') continue;
        const char *ext = nm + 64;
        if (strcmp(ext, N48X_EXT_SPV) && strcmp(ext, N48X_EXT_META) && strcmp(ext, N48X_EXT_FAIL)) continue;
        char sha[65]; memcpy(sha, nm, 64); sha[64] = 0;
        if (!n48x_sha_ok(sha)) continue;
        char p[N48X_PATH_MAX]; struct stat st;
        if (snprintf(p, sizeof p, "%s/%s", dir, nm) >= (int)sizeof p || lstat(p, &st) != 0 || !S_ISREG(st.st_mode)) continue;
        size_t i = 0; while (i < n && strcmp(e[i].sha, sha)) i++;
        if (i == n) {
            if (n == cap) { n48x_ent *g = (n48x_ent *)realloc(e, cap * 2 * sizeof *e); if (!g) break; e = g; cap *= 2; }
            memcpy(e[n].sha, sha, 65); e[n].mtime = 0; e[n].bytes = 0; n++;
        }
        const int64_t mt = (int64_t)st.st_mtimespec.tv_sec;
        if (mt > e[i].mtime) e[i].mtime = mt;
        e[i].bytes += (uint64_t)st.st_size;
    }
    closedir(d);
    unsigned char *del = (unsigned char *)calloc(n ? n : 1, 1);
    size_t removed = 0;
    if (del && n48x_trim_select(e, n, capBytes, del)) {
        for (size_t i = 0; i < n; i++) if (del[i]) {
            static const char *const exts[] = { N48X_EXT_META, N48X_EXT_SPV, N48X_EXT_FAIL };
            for (size_t x = 0; x < 3; x++) { char p[N48X_PATH_MAX]; struct stat st; if (n48x_file_path(p, sizeof p, dir, e[i].sha, exts[x]) && lstat(p, &st) == 0 && S_ISREG(st.st_mode)) (void)unlink(p); }
            removed++;
        }
    }
    free(del); free(e);
    return removed;
}
// Modification time touched on a cache hit (the caller does it once per sha per process).
static inline void n48x_touch(const char *path) { (void)utimes(path, NULL); }

// ======================================================================================================================================================================
// 7. Mach-O UUID of an image in memory (the translator library is read from its file; a loaded reader through dladdr)
// ======================================================================================================================================================================
static inline int n48x_macho_uuid(const void *mh, size_t avail, uint8_t out[16]) {
    if (!mh || avail < sizeof(struct mach_header_64)) return 0;
    const struct mach_header_64 *h = (const struct mach_header_64 *)mh;
    if (h->magic != MH_MAGIC_64) return 0;
    const uint8_t *p = (const uint8_t *)mh + sizeof *h;
    size_t left = avail - sizeof *h; if (left > h->sizeofcmds) left = h->sizeofcmds;
    for (uint32_t i = 0; i < h->ncmds; i++) {
        if (left < sizeof(struct load_command)) return 0;
        const struct load_command *lc = (const struct load_command *)p;
        if (lc->cmdsize < sizeof *lc || lc->cmdsize > left) return 0;
        if (lc->cmd == LC_UUID && lc->cmdsize >= sizeof(struct uuid_command)) { memcpy(out, ((const struct uuid_command *)lc)->uuid, 16); return 1; }
        p += lc->cmdsize; left -= lc->cmdsize;
    }
    return 0;
}
// LC_UUID of the thin 64-bit Mach-O file at path (reads the first 64 KiB; the load commands of a dylib are far smaller).
static inline int n48x_file_uuid(const char *path, uint8_t out[16]) {
    const int fd = open(path, O_RDONLY | O_NOFOLLOW);
    if (fd < 0) return 0;
    uint8_t *b = (uint8_t *)malloc(65536);
    if (!b) { close(fd); return 0; }
    const ssize_t n = pread(fd, b, 65536, 0);
    close(fd);
    const int ok = n > 0 && n48x_macho_uuid(b, (size_t)n, out);
    free(b);
    return ok;
}

// ======================================================================================================================================================================
// 8. The worker thread (section 1.5): created with a 64 MiB stack
// ======================================================================================================================================================================
static inline int n48x_spawn(pthread_t *t, void *(*fn)(void *), void *arg, size_t stackBytes) {
    pthread_attr_t a;
    if (pthread_attr_init(&a) != 0) return -1;
    int rc = pthread_attr_setstacksize(&a, stackBytes);
    if (rc == 0) { (void)pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED); rc = pthread_create(t, &a, fn, arg); }
    pthread_attr_destroy(&a);
    return rc;
}

// ======================================================================================================================================================================
// 9. Small file helpers: atomic install, read
// ======================================================================================================================================================================
// <path>.tmp<pid> -> fchmod 0644 -> rename. 0 = installed.
static inline int n48x_write_atomic(const char *path, const void *data, size_t n) {
    char tmp[N48X_PATH_MAX + 32];
    if (snprintf(tmp, sizeof tmp, "%s.tmp%d", path, (int)getpid()) >= (int)sizeof tmp) return -1;
    const int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600);
    if (fd < 0) return -1;
    const char *p = (const char *)data; size_t left = n; int ok = 1;
    while (left) { const ssize_t w = write(fd, p, left); if (w <= 0) { ok = 0; break; } p += w; left -= (size_t)w; }
    if (ok && fchmod(fd, 0644) != 0) ok = 0;
    if (close(fd) != 0) ok = 0;
    if (!ok || rename(tmp, path) != 0) { (void)unlink(tmp); return -1; }
    return 0;
}
__attribute__((format(printf, 3, 0))) static inline void n48x_set_err(char *err, size_t n, const char *fmt, const char *a) { if (err && n) snprintf(err, n, fmt, a ? a : ""); }

// ======================================================================================================================================================================
// 10. Dependencies of one translation (the reader and the translator), so tests can inject fakes and the real ones are loaded lazily
// ======================================================================================================================================================================
typedef struct { const char *symbol; const uint8_t *air; size_t airLen; } n48x_ldep;       // a linked dependency as the caller has it: logical name + AIR bitcode
typedef struct { const char *symbol; const char *ll; size_t llLen; } n48x_ldep_ll;           // ... after the reader: name + LLVM text
typedef struct {
    void *ctx;
    // Bitcode -> LLVM text (malloc'ed by the reader; release with free_text). 0 = ok.
    int (*air_to_text)(void *ctx, const uint8_t *air, size_t n, char **text, size_t *len, char *err, size_t errn);
    void (*free_text)(void *ctx, char *text);
    // LLVM text -> SPIR-V + trimmed meta (the library's n48x_translate). 0 ok, 1 refused, 2 panic caught, 3 bad arguments.
    int (*translate)(void *ctx, const char *ll, size_t llLen, const uint32_t *local, uint8_t **spv, size_t *spvLen, uint8_t **meta, size_t *metaLen, char *err, size_t errn);
    void (*release)(void *ctx, uint8_t *p, size_t n);
    // build 18 (P3): LLVM text + the authored dependencies' text -> SPIR-V + meta (the library's n48x_translate_linked). Same codes as translate. NULL = linked translation unavailable.
    int (*translate_linked)(void *ctx, const char *ll, size_t llLen, const uint32_t *local, const n48x_ldep_ll *deps, size_t nd, uint8_t **spv, size_t *spvLen, uint8_t **meta, size_t *metaLen, char *err, size_t errn);
} n48x_deps;

// ---- the crash-test hook: compiled in only with -DN48X_TESTHOOKS (test-xlate.c); the shipped bundle has none ----
#ifdef N48X_TESTHOOKS
#define N48X_HOOK_AFTER_MARKER() do { const char *h_ = getenv("N48X_TEST_EXIT_AFTER_MARKER"); if (h_ && h_[0] == '1') _exit(77); } while (0)
#else
#define N48X_HOOK_AFTER_MARKER() do { } while (0)
#endif

// One translation, run on the worker (or directly by a test). Order matters and is pinned by test-xlate.c:
//   caps -> plan against the markers -> write <sha>.inflight (O_EXCL) -> bitcode to text -> text cap -> translate -> caps -> .meta.json -> .spv -> remove the marker -> trim.
// A refusal (reader, translator, a cap) writes <sha>.fail; an I/O problem writes nothing. The marker is removed on every path that returns (a crash leaves it: that is the point).
static inline int n48x_translate_job_x(const n48x_deps *dp, const char *dir, const char *sha, const char *fname, const uint8_t *air, size_t airLen, const n48x_ldep *ld, size_t nld, char *err, size_t errn) {
    if (err && errn) err[0] = 0;
    if (!dp || !dp->air_to_text || !dp->translate || !dp->free_text || !dp->release) return N48X_E_UNAVAILABLE;
    if (!n48x_air_size_ok(airLen) || !air) { n48x_set_err(err, errn, "bitcode size outside 4 bytes .. 4 MiB%s", ""); return N48X_E_CAP; }
    if (!n48x_sha_ok(sha) || !dir || !dir[0]) { n48x_set_err(err, errn, "bad sha or directory%s", ""); return N48X_E_IO; }
    {   // P4 (build 16): a repeat of a translation that has already been installed (a caller that missed the running job by a hair) is answered, not run again: the .spv is written LAST, so its presence means the pair is complete
        char done[N48X_PATH_MAX]; struct stat dst;
        if (n48x_file_path(done, sizeof done, dir, sha, N48X_EXT_SPV) && lstat(done, &dst) == 0 && S_ISREG(dst.st_mode)) return N48X_OK;
    }
    switch (n48x_probe(dir, sha)) {
    case N48X_PL_SKIP_CRASH: case N48X_PL_PROMOTE_THEN_SKIP: n48x_set_err(err, errn, "a translation of this shader crashed a process before (.crash)%s", ""); return N48X_E_SKIP_CRASH;
    case N48X_PL_SKIP_FAIL:  n48x_set_err(err, errn, "the translator refused this shader before (.fail)%s", ""); return N48X_E_SKIP_FAIL;
    case N48X_PL_SKIP_LIVE:  n48x_set_err(err, errn, "another process is translating this shader%s", ""); return N48X_E_SKIP_LIVE;
    default: break;
    }
    const int mk = n48x_marker_create(dir, sha);
    if (mk == 1) { n48x_set_err(err, errn, "another process is translating this shader%s", ""); return N48X_E_SKIP_LIVE; }
    if (mk != 0) { n48x_set_err(err, errn, "cannot create the crash marker%s", ""); return N48X_E_IO; }
    N48X_HOOK_AFTER_MARKER();
    char path[N48X_PATH_MAX], failPath[N48X_PATH_MAX], why[512]; why[0] = 0;
    (void)n48x_file_path(failPath, sizeof failPath, dir, sha, N48X_EXT_FAIL);
    char *text = NULL; size_t tl = 0; uint8_t *spv = NULL, *meta = NULL; size_t sl = 0, ml = 0; int rc = N48X_OK;
    n48x_ldep_ll *dl = NULL; char **dtext = NULL; size_t ndt = 0;
    if (dp->air_to_text(dp->ctx, air, airLen, &text, &tl, why, sizeof why) != 0 || !text) { rc = N48X_E_READER; goto refused; }
    if (!n48x_ll_size_ok(tl)) { snprintf(why, sizeof why, "LLVM text of %zu bytes is outside the cap", tl); rc = N48X_E_CAP; goto refused; }
    if (nld) {   // build 18 (P3): every dependency goes through the SAME reader as the entry; the library then sanitises each one like the entry
        if (!dp->translate_linked || nld > N48X_MAX_DEPS || !ld) { snprintf(why, sizeof why, "linked translation unavailable or too many dependencies (%zu)", nld); rc = N48X_E_UNAVAILABLE; goto refused; }
        dl = (n48x_ldep_ll *)calloc(nld, sizeof *dl); dtext = (char **)calloc(nld, sizeof *dtext);
        if (!dl || !dtext) { snprintf(why, sizeof why, "out of memory"); rc = N48X_E_IO; goto refused; }
        size_t tot = tl;
        for (size_t i = 0; i < nld; i++) {
            char *t = NULL; size_t l = 0;
            if (!ld[i].symbol || !ld[i].symbol[0] || !n48x_air_size_ok(ld[i].airLen) || !ld[i].air) { snprintf(why, sizeof why, "dependency %zu: bad name or bitcode size", i); rc = N48X_E_CAP; goto refused; }
            if (dp->air_to_text(dp->ctx, ld[i].air, ld[i].airLen, &t, &l, why, sizeof why) != 0 || !t) { rc = N48X_E_READER; goto refused; }
            dtext[ndt++] = t; tot += l;
            if (!n48x_ll_size_ok(l) || tot > N48X_MAX_LL) { snprintf(why, sizeof why, "LLVM text of the dependencies (%zu bytes) is outside the cap", tot); rc = N48X_E_CAP; goto refused; }
            dl[i].symbol = ld[i].symbol; dl[i].ll = t; dl[i].llLen = l;
        }
    }
    {
        const uint32_t *loc = n48x_local_for(fname);
        const int tr = nld ? dp->translate_linked(dp->ctx, text, tl, loc, dl, nld, &spv, &sl, &meta, &ml, why, sizeof why)
                           : dp->translate(dp->ctx, text, tl, loc, &spv, &sl, &meta, &ml, why, sizeof why);
        dp->free_text(dp->ctx, text); text = NULL;
        for (size_t i = 0; i < ndt; i++) dp->free_text(dp->ctx, dtext[i]);
        ndt = 0; free(dl); free(dtext); dl = NULL; dtext = NULL;
        if (tr != 0) { rc = N48X_E_REFUSED; goto refused; }
    }
    if (!n48x_spv_size_ok(sl) || !n48x_meta_size_ok(ml)) { snprintf(why, sizeof why, "SPIR-V %zu bytes / meta %zu bytes outside the caps", sl, ml); rc = N48X_E_CAP; goto refused; }
    // .meta.json FIRST, .spv LAST: the lookup keys on the .spv, so a reader that sees it always finds the meta.
    if (!n48x_file_path(path, sizeof path, dir, sha, N48X_EXT_META) || n48x_write_atomic(path, meta, ml) != 0) { rc = N48X_E_IO; snprintf(why, sizeof why, "cannot write the meta file"); goto done; }
    if (!n48x_file_path(path, sizeof path, dir, sha, N48X_EXT_SPV) || n48x_write_atomic(path, spv, sl) != 0) { rc = N48X_E_IO; snprintf(why, sizeof why, "cannot write the spv file"); goto done; }
    goto done;
refused:
    if (text) { dp->free_text(dp->ctx, text); text = NULL; }
    for (size_t i = 0; i < ndt; i++) dp->free_text(dp->ctx, dtext[i]);
    free(dl); free(dtext); dl = NULL; dtext = NULL; ndt = 0;
    (void)n48x_write_atomic(failPath, why, strlen(why));
done:
    if (spv) dp->release(dp->ctx, spv, sl);
    if (meta) dp->release(dp->ctx, meta, ml);
    n48x_marker_remove(dir, sha);
    if (rc == N48X_OK) (void)n48x_trim_dir(dir, N48X_CACHE_CAP);
    if (err && errn && why[0]) snprintf(err, errn, "%s", why);
    return rc;
}
// The unlinked job (every caller before build 18).
static inline int n48x_translate_job(const n48x_deps *dp, const char *dir, const char *sha, const char *fname, const uint8_t *air, size_t airLen, char *err, size_t errn) {
    return n48x_translate_job_x(dp, dir, sha, fname, air, airLen, NULL, 0, err, errn);
}

// ======================================================================================================================================================================
// 11. The reader: Apple's LLVM from the OS (section 1.1, option A)
// ======================================================================================================================================================================
typedef struct {
    void *h; char path[N48X_PATH_MAX];
    void *(*ContextCreate)(void);
    void (*ContextDispose)(void *);
    void (*ContextSetDiagnosticHandler)(void *, void (*)(void *, void *), void *);
    void *(*CreateMemoryBufferWithMemoryRangeCopy)(const char *, size_t, const char *);
    void (*DisposeMemoryBuffer)(void *);
    int (*ParseBitcodeInContext2)(void *, void *, void **);
    char *(*PrintModuleToString)(void *);
    void (*DisposeModule)(void *);
    void (*DisposeMessage)(char *);
    char *(*GetDiagInfoDescription)(void *);
    void (*SetOpaquePointers)(void *, int);   // OPTIONAL 11th symbol, llvm::LLVMContext::setOpaquePointers(bool) const (LLVM 15/16's public C++ method, exported by GPUCompiler's libLLVM): see n48x_reader_bind
} n48x_reader;

static inline int n48x_reader_bind(n48x_reader *r, void *h) {
#define N48X_SYM(F) do { *(void **)&r->F = dlsym(h, "LLVM" #F); if (!r->F) return 0; } while (0)
    N48X_SYM(ContextCreate); N48X_SYM(ContextDispose); N48X_SYM(ContextSetDiagnosticHandler); N48X_SYM(CreateMemoryBufferWithMemoryRangeCopy); N48X_SYM(DisposeMemoryBuffer);
    N48X_SYM(ParseBitcodeInContext2); N48X_SYM(PrintModuleToString); N48X_SYM(DisposeModule); N48X_SYM(DisposeMessage); N48X_SYM(GetDiagInfoDescription);
#undef N48X_SYM
    // Apple's GPUCompiler LLVM is an older LLVM that prints TYPED pointers (`%struct._sampler_t addrspace(2)*`, `bitcast ... to ...` constants) unless the context is switched to opaque pointers before
    // the module is read; the translator reads LLVM 23's text (`ptr addrspace(2)`). The switch is the public C++ method, found by its mangled name; a library without it (an LLVM 17+, which is
    // opaque-only, or a future OS) simply prints what it prints.
    *(void **)&r->SetOpaquePointers = dlsym(h, "_ZNK4llvm11LLVMContext17setOpaquePointersEb");
    r->h = h;
    return 1;
}
static inline int n48x_num_desc(const void *a, const void *b) {
    const unsigned long x = strtoul(*(const char *const *)a, NULL, 10), y = strtoul(*(const char *const *)b, NULL, 10);
    return x < y ? 1 : (x > y ? -1 : 0);
}
// Candidate paths, in order: Versions/Current, then every numeric Versions/<n>/ directory (highest first). Returns the count (<= max).
static inline size_t n48x_reader_candidates(const char *base, char out[][N48X_PATH_MAX], size_t max) {
    size_t n = 0;
    if (n < max) snprintf(out[n++], N48X_PATH_MAX, "%s/Current/Libraries/libLLVM.dylib", base);
    DIR *d = opendir(base);
    if (!d) return n;
    char *names[64]; size_t nn = 0; struct dirent *e;
    while ((e = readdir(d)) != NULL && nn < 64) {
        const char *s = e->d_name; size_t i = 0;
        if (!*s) continue;
        while (s[i] >= '0' && s[i] <= '9') i++;
        if (s[i] != 0 || i == 0 || i > 12) continue;
        names[nn++] = strdup(s);
    }
    closedir(d);
    qsort(names, nn, sizeof names[0], n48x_num_desc);
    for (size_t i = 0; i < nn; i++) { if (n < max) snprintf(out[n++], N48X_PATH_MAX, "%s/%s/Libraries/libLLVM.dylib", base, names[i]); free(names[i]); }
    return n;
}
// override != NULL: only that path (tests; the bundle honours it for N48M_ALLOW root tools only). Otherwise the candidates under base. 0 = loaded.
static inline int n48x_reader_load(n48x_reader *r, const char *base, const char *override, char *err, size_t errn) {
    memset(r, 0, sizeof *r);
    char cand[16][N48X_PATH_MAX]; size_t n = 0;
    if (override && override[0]) { snprintf(cand[0], N48X_PATH_MAX, "%s", override); n = 1; }
    else n = n48x_reader_candidates(base, cand, 16);
    for (size_t i = 0; i < n; i++) {
        void *h = dlopen(cand[i], RTLD_NOW | RTLD_LOCAL);
        if (!h) continue;
        if (n48x_reader_bind(r, h)) { snprintf(r->path, sizeof r->path, "%s", cand[i]); return 0; }
        dlclose(h);
    }
    n48x_set_err(err, errn, "no usable LLVM bitcode reader (%s)", override && override[0] ? override : base);
    return -1;
}
typedef struct { const n48x_reader *r; char msg[256]; } n48x_diag;
static inline void n48x_diag_cb(void *info, void *opaque) {
    n48x_diag *d = (n48x_diag *)opaque;
    if (d->msg[0] || !info) return;
    char *s = d->r->GetDiagInfoDescription(info);
    if (s) { snprintf(d->msg, sizeof d->msg, "%s", s); d->r->DisposeMessage(s); }
}
static inline int n48x_reader_print(const n48x_reader *r, const uint8_t *air, size_t n, char **text, size_t *len, char *err, size_t errn) {
    *text = NULL; *len = 0;
    n48x_diag dg; dg.r = r; dg.msg[0] = 0;
    void *ctx = r->ContextCreate();
    if (!ctx) { n48x_set_err(err, errn, "LLVMContextCreate failed%s", ""); return -1; }
    if (r->SetOpaquePointers) r->SetOpaquePointers(ctx, 1);   // before anything is read into the context
    r->ContextSetDiagnosticHandler(ctx, n48x_diag_cb, &dg);
    void *buf = r->CreateMemoryBufferWithMemoryRangeCopy((const char *)air, n, "air");
    void *mod = NULL; int rc = -1;
    if (buf && r->ParseBitcodeInContext2(ctx, buf, &mod) == 0 && mod) {
        char *s = r->PrintModuleToString(mod);
        if (s) {
            const size_t l = strlen(s);
            char *copy = (char *)malloc(l + 1);   // the library's string is released with LLVMDisposeMessage; hand out our own so the caller frees it with free()
            if (copy) { memcpy(copy, s, l + 1); *text = copy; *len = l; rc = 0; }
            r->DisposeMessage(s);
        }
    } else n48x_set_err(err, errn, "bitcode not readable: %s", dg.msg[0] ? dg.msg : "parse failed");
    if (mod) r->DisposeModule(mod);
    if (buf) r->DisposeMemoryBuffer(buf);
    r->ContextDispose(ctx);
    return rc;
}

// ======================================================================================================================================================================
// 12. The translator library (Resources/libn48xlate.dylib)
// ======================================================================================================================================================================
typedef struct {
    void *h;
    uint32_t (*abi_version)(void);
    int (*translate)(const uint8_t *, size_t, const uint32_t *, uint8_t **, size_t *, uint8_t **, size_t *, char *, size_t);
    void (*freep)(uint8_t *, size_t);
    int (*translate_linked)(const uint8_t *, size_t, const uint32_t *, const void *, size_t, uint8_t **, size_t *, uint8_t **, size_t *, char *, size_t);   // build 18 (P3), ABI 2
} n48x_xlib;
static inline int n48x_xlib_load(n48x_xlib *x, const char *path, char *err, size_t errn) {
    memset(x, 0, sizeof *x);
    void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!h) { n48x_set_err(err, errn, "cannot load the translator library %s", path); return -1; }
    *(void **)&x->abi_version = dlsym(h, "n48x_abi_version");
    *(void **)&x->translate = dlsym(h, "n48x_translate");
    *(void **)&x->freep = dlsym(h, "n48x_free");
    *(void **)&x->translate_linked = dlsym(h, "n48x_translate_linked");
    if (!x->abi_version || !x->translate || !x->freep || !x->translate_linked) { dlclose(h); memset(x, 0, sizeof *x); n48x_set_err(err, errn, "the translator library lacks the n48x_* entry points: %s", path); return -1; }
    if (x->abi_version() != N48X_ABI_VERSION) { dlclose(h); memset(x, 0, sizeof *x); n48x_set_err(err, errn, "the translator library has an unknown ABI version: %s", path); return -1; }
    x->h = h;
    return 0;
}

typedef struct { n48x_reader rd; n48x_xlib xl; } n48x_real;
static inline int n48x_real_air_to_text(void *c, const uint8_t *air, size_t n, char **t, size_t *l, char *e, size_t en) { return n48x_reader_print(&((n48x_real *)c)->rd, air, n, t, l, e, en); }
static inline void n48x_real_free_text(void *c, char *t) { (void)c; free(t); }
static inline int n48x_real_translate(void *c, const char *ll, size_t ll_len, const uint32_t *local, uint8_t **spv, size_t *sl, uint8_t **meta, size_t *ml, char *e, size_t en) {
    return ((n48x_real *)c)->xl.translate((const uint8_t *)ll, ll_len, local, spv, sl, meta, ml, e, en);
}
static inline void n48x_real_release(void *c, uint8_t *p, size_t n) { ((n48x_real *)c)->xl.freep(p, n); }
// The library's N48xDep: { symbol, symbol_len, ll, ll_len }.
typedef struct { const uint8_t *symbol; size_t symbol_len; const uint8_t *ll; size_t ll_len; } n48x_cdep;
static inline int n48x_real_translate_linked(void *c, const char *ll, size_t ll_len, const uint32_t *local, const n48x_ldep_ll *deps, size_t nd, uint8_t **spv, size_t *sl, uint8_t **meta, size_t *ml, char *e, size_t en) {
    n48x_cdep *cd = (n48x_cdep *)calloc(nd ? nd : 1, sizeof *cd);
    if (!cd) { n48x_set_err(e, en, "out of memory%s", ""); return 3; }
    for (size_t i = 0; i < nd; i++) { cd[i].symbol = (const uint8_t *)deps[i].symbol; cd[i].symbol_len = strlen(deps[i].symbol); cd[i].ll = (const uint8_t *)deps[i].ll; cd[i].ll_len = deps[i].llLen; }
    const int rc = ((n48x_real *)c)->xl.translate_linked((const uint8_t *)ll, ll_len, local, cd, nd, spv, sl, meta, ml, e, en);
    free(cd);
    return rc;
}
// Loads both libraries (reader override only for tests / root tools). 0 = ready; dp is filled.
static inline int n48x_real_open(n48x_real *r, n48x_deps *dp, const char *gpucBase, const char *readerOverride, const char *xlPath, char *err, size_t errn) {
    memset(r, 0, sizeof *r);
    if (n48x_reader_load(&r->rd, gpucBase, readerOverride, err, errn) != 0) return -1;
    if (n48x_xlib_load(&r->xl, xlPath, err, errn) != 0) return -1;
    dp->ctx = r; dp->air_to_text = n48x_real_air_to_text; dp->free_text = n48x_real_free_text; dp->translate = n48x_real_translate; dp->release = n48x_real_release; dp->translate_linked = n48x_real_translate_linked;
    return 0;
}

// ======================================================================================================================================================================
// 13. The engine: one worker thread, jobs one at a time, the caller waits up to a deadline (section 1.5)
// ======================================================================================================================================================================
typedef struct n48x_job {
    struct n48x_job *next;
    char dir[N48X_PATH_MAX], sha[65], name[128];
    uint8_t *air; size_t airLen;
    n48x_ldep *deps; size_t nd;   // build 18 (P3): owned copies (name + bitcode) of the linked dependencies
    int refs, done, rc; char err[256];
} n48x_job;
typedef struct n48x_engine {
    pthread_mutex_t mu; pthread_cond_t cvWork, cvDone;
    n48x_job *head, *tail, *running;
    int started, tried, ready; size_t stack;
    n48x_deps deps; n48x_real real;
    int useReal; char gpucBase[N48X_PATH_MAX], readerOverride[N48X_PATH_MAX], xlPath[N48X_PATH_MAX];
    char sweptDir[N48X_PATH_MAX];
    char loadErr[256];
} n48x_engine;

// deps != NULL: use those (tests). deps == NULL: load the real reader and translator on the worker thread, on the first job. stack: the worker's stack size (N48X_WORKER_STACK in the bundle).
static inline void n48x_engine_init(n48x_engine *e, const n48x_deps *deps, const char *gpucBase, const char *readerOverride, const char *xlPath, size_t stack) {
    memset(e, 0, sizeof *e);
    pthread_mutex_init(&e->mu, NULL); pthread_cond_init(&e->cvWork, NULL); pthread_cond_init(&e->cvDone, NULL);
    e->stack = stack;
    if (deps) { e->deps = *deps; e->ready = 1; }
    else {
        e->useReal = 1;
        snprintf(e->gpucBase, sizeof e->gpucBase, "%s", gpucBase ? gpucBase : N48X_GPUC_BASE);
        snprintf(e->readerOverride, sizeof e->readerOverride, "%s", readerOverride ? readerOverride : "");
        snprintf(e->xlPath, sizeof e->xlPath, "%s", xlPath ? xlPath : "");
    }
}
static inline void n48x_job_unref(n48x_engine *e, n48x_job *j) {   // caller holds e->mu
    (void)e;
    if (--j->refs == 0) { for (size_t i = 0; i < j->nd; i++) { free((void *)j->deps[i].symbol); free((void *)j->deps[i].air); } free(j->deps); free(j->air); free(j); }
}
static inline void *n48x_worker_main(void *arg) {
    n48x_engine *e = (n48x_engine *)arg;
    pthread_mutex_lock(&e->mu);
    for (;;) {
        while (!e->head) pthread_cond_wait(&e->cvWork, &e->mu);
        n48x_job *j = e->head; e->head = j->next; if (!e->head) e->tail = NULL;
        e->running = j;
        if (!e->ready && !e->tried) {   // first job: load the reader and the translator, here, off the caller's thread
            e->tried = 1;
            pthread_mutex_unlock(&e->mu);
            const int lr = n48x_real_open(&e->real, &e->deps, e->gpucBase, e->readerOverride[0] ? e->readerOverride : NULL, e->xlPath, e->loadErr, sizeof e->loadErr);
            pthread_mutex_lock(&e->mu);
            e->ready = (lr == 0);
        }
        pthread_mutex_unlock(&e->mu);
        int rc;
        if (!e->ready) { rc = N48X_E_UNAVAILABLE; snprintf(j->err, sizeof j->err, "%s", e->loadErr[0] ? e->loadErr : "translator unavailable"); }
        else {
            if (strcmp(e->sweptDir, j->dir) != 0) { (void)n48x_sweep(j->dir); snprintf(e->sweptDir, sizeof e->sweptDir, "%s", j->dir); }   // the first miss in a directory: dead markers become .crash
            rc = n48x_translate_job_x(&e->deps, j->dir, j->sha, j->name, j->air, j->airLen, j->deps, j->nd, j->err, sizeof j->err);
        }
        pthread_mutex_lock(&e->mu);
        j->rc = rc; j->done = 1; e->running = NULL;
        pthread_cond_broadcast(&e->cvDone);
        n48x_job_unref(e, j);
    }
    return NULL;
}
static inline n48x_job *n48x_find_job(n48x_engine *e, const char *dir, const char *sha) {   // caller holds e->mu
    for (n48x_job *j = e->head; j; j = j->next) if (!strcmp(j->sha, sha) && !strcmp(j->dir, dir)) return j;
    if (e->running && !strcmp(e->running->sha, sha) && !strcmp(e->running->dir, dir)) return e->running;
    return NULL;
}
// Submits one translation and waits up to waitMs (0 = do not wait; the worker still finishes it and installs the result). A second request for a sha already queued or running shares that job.
// Returns the job's code, or N48X_E_TIMEOUT when the wait ran out. err (optional) gets a message.
static inline int n48x_engine_run_linked(n48x_engine *e, const char *dir, const char *sha, const char *fname, const uint8_t *air, size_t airLen, const n48x_ldep *ld, size_t nld, int waitMs, char *err, size_t errn) {
    if (err && errn) err[0] = 0;
    if (nld > N48X_MAX_DEPS || (nld && !ld)) { n48x_set_err(err, errn, "too many linked dependencies%s", ""); return N48X_E_CAP; }
    { size_t tot = 0; for (size_t i = 0; i < nld; i++) { tot += ld[i].airLen; if (!ld[i].symbol || !ld[i].symbol[0] || !n48x_air_size_ok(ld[i].airLen) || !ld[i].air || tot > N48X_MAX_DEP_AIR) { if (err && errn) snprintf(err, errn, "linked dependency %zu: bad name or bitcode outside the caps", i); return N48X_E_CAP; } } }
    if (!n48x_air_size_ok(airLen) || !air) { n48x_set_err(err, errn, "bitcode size outside 4 bytes .. 4 MiB%s", ""); return N48X_E_CAP; }
    if (!n48x_sha_ok(sha) || !dir || strlen(dir) >= N48X_PATH_MAX) return N48X_E_IO;
    pthread_mutex_lock(&e->mu);
    n48x_job *j = n48x_find_job(e, dir, sha);
    if (j) j->refs++;
    else {
        j = (n48x_job *)calloc(1, sizeof *j);
        uint8_t *copy = (uint8_t *)malloc(airLen);
        if (!j || !copy) { free(j); free(copy); pthread_mutex_unlock(&e->mu); return N48X_E_IO; }
        memcpy(copy, air, airLen);
        j->air = copy; j->airLen = airLen; j->refs = 2;   // the waiter and the worker
        if (nld) {   // build 18 (P3): the worker outlives the caller's arrays
            j->deps = (n48x_ldep *)calloc(nld, sizeof *j->deps);
            int bad = j->deps == NULL;
            for (size_t i = 0; i < nld && !bad; i++) {
                char *sy = strdup(ld[i].symbol); uint8_t *ab = (uint8_t *)malloc(ld[i].airLen);
                if (!sy || !ab) { free(sy); free(ab); bad = 1; break; }
                memcpy(ab, ld[i].air, ld[i].airLen); j->deps[i].symbol = sy; j->deps[i].air = ab; j->deps[i].airLen = ld[i].airLen; j->nd = i + 1;
            }
            if (bad) { j->refs = 1; n48x_job_unref(e, j); pthread_mutex_unlock(&e->mu); return N48X_E_IO; }
        }
        snprintf(j->dir, sizeof j->dir, "%s", dir); memcpy(j->sha, sha, 65); snprintf(j->name, sizeof j->name, "%s", fname ? fname : "");
        if (!e->started) {
            pthread_t t;
            if (n48x_spawn(&t, n48x_worker_main, e, e->stack) != 0) { j->refs = 1; n48x_job_unref(e, j); pthread_mutex_unlock(&e->mu); return N48X_E_IO; }
            e->started = 1;
        }
        if (e->tail) e->tail->next = j; else e->head = j;
        e->tail = j;
        pthread_cond_signal(&e->cvWork);
    }
    int rc = N48X_E_TIMEOUT;
    if (waitMs > 0) {
        struct timeval tv; gettimeofday(&tv, NULL);
        struct timespec ts; ts.tv_sec = tv.tv_sec + waitMs / 1000; ts.tv_nsec = (long)tv.tv_usec * 1000 + (long)(waitMs % 1000) * 1000000L;
        if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
        while (!j->done) { if (pthread_cond_timedwait(&e->cvDone, &e->mu, &ts) == ETIMEDOUT) break; }
    }
    if (j->done) { rc = j->rc; if (err && errn) snprintf(err, errn, "%s", j->err); }
    n48x_job_unref(e, j);
    pthread_mutex_unlock(&e->mu);
    return rc;
}
// The unlinked request (every caller before build 18).
static inline int n48x_engine_run(n48x_engine *e, const char *dir, const char *sha, const char *fname, const uint8_t *air, size_t airLen, int waitMs, char *err, size_t errn) {
    return n48x_engine_run_linked(e, dir, sha, fname, air, airLen, NULL, 0, waitMs, err, errn);
}

#endif /* N48_XLATE_H */
