// test-xlate-linked-real.c: the linked translation on REAL AIR from Apple's own compiler, through the REAL reader (the OS's GPUCompiler LLVM) and the REAL libn48xlate. Driven by test-xlate-linked-real.sh.
//   usage: test-xlate-linked-real <libn48xlate.dylib> <entry.air> <dep.air> <dep symbol>
// Checks: the entry alone is REFUSED by the translator for its visible function reference (so the unlinked path never produced a pipeline); the same entry with its dependency translates and the SPIR-V holds
// the dependency's arithmetic; the dependency's own bitcode alone is not a stage; the library speaks ABI 2.
#include "n48_xlate.h"
static int fails;
#define CHECK(name, cond) do { int ok_ = (cond) ? 1 : 0; printf("%s %s\n", ok_ ? "ok  " : "FAIL", name); if (!ok_) fails++; } while (0)
static uint8_t *slurp(const char *p, size_t *n) { FILE *f = fopen(p, "rb"); if (!f) return NULL; fseek(f, 0, SEEK_END); *n = (size_t)ftell(f); fseek(f, 0, SEEK_SET); uint8_t *b = malloc(*n); if (fread(b, 1, *n, f) != *n) { free(b); b = NULL; } fclose(f); return b; }
int main(int argc, char **argv) {
    if (argc < 5) { puts("usage"); return 2; }
    char tmpl[] = "/private/tmp/n48xlate-real.XXXXXX"; char *d = mkdtemp(tmpl); if (!d) return 2;
    size_t na = 0, nd = 0; uint8_t *entry = slurp(argv[2], &na), *dep = slurp(argv[3], &nd);
    if (!entry || !dep) { puts("cannot read the AIR"); return 2; }
    n48x_real r; n48x_deps dp; char err[512] = "";
    if (n48x_real_open(&r, &dp, N48X_GPUC_BASE, NULL, argv[1], err, sizeof err) != 0) { printf("FAIL open: %s\n", err); return 1; }
    CHECK("the library speaks ABI 2", r.xl.abi_version() == 2);
    char sha1[65], sha2[65], sha3[65];
    uint8_t h[CC_SHA256_DIGEST_LENGTH]; CC_SHA256(entry, (CC_LONG)na, h); n48x_hex(sha1, h, 32);
    int rc = n48x_translate_job_x(&dp, d, sha1, "fs", entry, na, NULL, 0, err, sizeof err);
    CHECK("the entry ALONE is refused (its visible function reference has no body)", rc == N48X_E_REFUSED && strstr(err, "visible function reference") != NULL);
    printf("     unlinked refusal: %s\n", err);
    char key[65]; const char *sy[] = { argv[4] }; const char *sh[1]; CC_SHA256(dep, (CC_LONG)nd, h); n48x_hex(sha3, h, 32); sh[0] = sha3; n48x_link_sha(key, sha1, 1, sy, sh);
    n48x_ldep ld[1] = { { argv[4], dep, nd } };
    rc = n48x_translate_job_x(&dp, d, key, "fs", entry, na, ld, 1, err, sizeof err);
    CHECK("the entry WITH its dependency translates", rc == N48X_OK);
    if (rc != N48X_OK) printf("     refusal: %s\n", err);
    char p[1100]; size_t sl = 0; uint8_t *spv = NULL;
    if (rc == N48X_OK && n48x_file_path(p, sizeof p, d, key, N48X_EXT_SPV)) spv = slurp(p, &sl);
    CHECK("the SPIR-V is a module", spv && sl >= 20 && spv[0] == 3 && spv[1] == 2 && spv[2] == 0x23 && spv[3] == 7);
    int fmul = 0, fadd = 0;   // OpFMul = 133, OpFAdd = 129: the dependency computes c * 0.5 + 0.25
    for (size_t pw = 5; spv && pw < sl / 4;) { uint32_t w = ((uint32_t *)spv)[pw]; uint32_t op = w & 0xffff, wc = w >> 16; if (!wc) break; if (op == 133) fmul++; if (op == 129) fadd++; pw += wc; }
    CHECK("the dependency's arithmetic (OpFMul and OpFAdd) is in the fragment shader", fmul >= 1 && fadd >= 1);
    sha2[0] = 0; (void)sha2;
    char meta[1100]; size_t ml = 0; uint8_t *mj = NULL; if (n48x_file_path(meta, sizeof meta, d, key, N48X_EXT_META)) mj = slurp(meta, &ml);
    CHECK("the reflection names a Fragment stage", mj && ml > 2 && memmem(mj, ml, "Fragment", 8) != NULL);
    rc = n48x_translate_job_x(&dp, d, "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff", "shade", dep, nd, NULL, 0, err, sizeof err);
    CHECK("the dependency's own bitcode is not a stage and is refused", rc == N48X_E_REFUSED);
    char junk[600]; snprintf(junk, sizeof junk, "%s", d);
    // clean up our own scratch directory (files created by the job only)
    DIR *dd = opendir(d); struct dirent *e; if (dd) { while ((e = readdir(dd))) { if (e->d_name[0] == '.') continue; char q[1400]; snprintf(q, sizeof q, "%s/%s", d, e->d_name); unlink(q); } closedir(dd); }
    if (strstr(d, "/n48xlate-real.") ) rmdir(d);
    printf("%s (%d failure(s))\n", fails ? "FAILED" : "all passed", fails);
    return fails ? 1 : 0;
}
