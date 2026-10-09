// test-cienv.c: host test of n48_cienv.h (bundle 9, CI_USE_MTL_DAG_FOR_CIKL_SRC), plus source pins that tie the REAL Navi48Device.m to it.
// Build/run (from this directory):  cc -O1 -Wall -Wextra -Werror -o /tmp/test-cienv test-cienv.c && /tmp/test-cienv .
//   argv[1] = the directory holding Navi48Device.m and n48_cienv.h (default ".").
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "n48_cienv.h"
static int fails, checks;
#define CHECK(c, ...) do { int ok_ = (c) ? 1 : 0; checks++; printf("%s: ", ok_ ? "ok  " : "FAIL"); printf(__VA_ARGS__); printf("\n"); if (!ok_) fails++; } while (0)
// a fake environment: the test never touches the real one
static char *fake_val; static int fake_set_calls, fake_last_overwrite = -1, fake_set_rc;
static const char *fgetenv(const char *n) { return strcmp(n, N48CE_VAR) == 0 ? fake_val : NULL; }
static int fsetenv(const char *n, const char *v, int ow) { fake_set_calls++; fake_last_overwrite = ow; if (fake_set_rc) return fake_set_rc; if (strcmp(n, N48CE_VAR)) return -1; if (!ow && fake_val) return 0; free(fake_val); fake_val = strdup(v); return 0; }
static void reset(const char *pre) { free(fake_val); fake_val = pre ? strdup(pre) : NULL; fake_set_calls = 0; fake_last_overwrite = -1; fake_set_rc = 0; }
static char *slurp(const char *dir, const char *name) {
    char p[1024]; snprintf(p, sizeof p, "%s/%s", dir, name); FILE *f = fopen(p, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET); char *b = calloc(1, (size_t)n + 1); if (fread(b, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(b); return NULL; } fclose(f); return b; }
int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : ".";
    CHECK(strcmp(N48CE_VAR, "CI_USE_MTL_DAG_FOR_CIKL_SRC") == 0, "the variable is CI_USE_MTL_DAG_FOR_CIKL_SRC");
    reset(NULL); int r = n48ce_apply(fgetenv, fsetenv);
    CHECK(r == N48CE_SET && fake_val && !strcmp(fake_val, "0"), "unset -> set to \"0\" (returned %d, value %s)", r, fake_val ? fake_val : "(null)");
    r = n48ce_apply(fgetenv, fsetenv);
    CHECK(r == N48CE_ALREADY_SET && fake_set_calls == 1 && !strcmp(fake_val, "0"), "a second call changes nothing (returned %d, %d setenv calls in total)", r, fake_set_calls);
    reset("1"); r = n48ce_apply(fgetenv, fsetenv);
    CHECK(r == N48CE_ALREADY_SET && fake_val && !strcmp(fake_val, "1") && fake_set_calls == 0, "preset \"1\" -> unchanged, setenv not called (the ciprobe control case relies on this)");
    reset("0"); r = n48ce_apply(fgetenv, fsetenv);
    CHECK(r == N48CE_ALREADY_SET && fake_val && !strcmp(fake_val, "0") && fake_set_calls == 0, "preset \"0\" -> unchanged, setenv not called");
    reset(""); r = n48ce_apply(fgetenv, fsetenv);
    CHECK(r == N48CE_ALREADY_SET && fake_val && fake_val[0] == 0 && fake_set_calls == 0, "preset empty string -> unchanged (a value is a value)");
    reset("yes"); r = n48ce_apply(fgetenv, fsetenv);
    CHECK(r == N48CE_ALREADY_SET && !strcmp(fake_val, "yes"), "preset \"yes\" -> unchanged");
    reset(NULL); r = n48ce_apply(fgetenv, fsetenv);
    CHECK(fake_last_overwrite == 0, "when it does set, overwrite is 0 (never replaces a value that appeared between the check and the set)");
    reset(NULL); fake_set_rc = -1; r = n48ce_apply(fgetenv, fsetenv);
    CHECK(r == N48CE_SET_FAILED, "a failing setenv is reported (returned %d)", r);
    // the real environment, in a child-free way: the process env is ours
    unsetenv(N48CE_VAR); r = n48ce_apply((n48ce_getenv_fn)getenv, setenv);
    CHECK(r == N48CE_SET && getenv(N48CE_VAR) && !strcmp(getenv(N48CE_VAR), "0"), "real getenv/setenv: unset -> \"0\"");
    setenv(N48CE_VAR, "1", 1); r = n48ce_apply((n48ce_getenv_fn)getenv, setenv);
    CHECK(r == N48CE_ALREADY_SET && !strcmp(getenv(N48CE_VAR), "1"), "real getenv/setenv: preset \"1\" survives");
    // ---- source pins on the REAL Navi48Device.m ----
    char *m = slurp(dir, "Navi48Device.m");
    CHECK(m != NULL, "Navi48Device.m is readable");
    if (m) {
        const char *inc = strstr(m, "#include \"n48_cienv.h\"");
        const char *ctor = strstr(m, "__attribute__((constructor)) static void n48_cienv_ctor(void)");
        CHECK(inc != NULL, "Navi48Device.m includes n48_cienv.h");
        CHECK(ctor != NULL, "Navi48Device.m has the constructor n48_cienv_ctor");
        if (ctor) {
            const char *end = strstr(ctor, "\n}\n");
            const char *call = strstr(ctor, "n48ce_apply((n48ce_getenv_fn)getenv, setenv)");
            const char *once = strstr(ctor, "N48_ONCE(");
            CHECK(end && call && call < end, "the constructor calls n48ce_apply with the real getenv / setenv");
            CHECK(end && once && once < end && strstr(ctor, "N48CE_ALREADY_SET") && strstr(ctor, "N48CE_SET"), "the constructor logs once, saying whether it set the variable or found one");
            CHECK(strstr(m, "n48ce_apply(") == call || strstr(strstr(m, "n48ce_apply(") + 12, "n48ce_apply(") == NULL, "n48ce_apply is called from exactly one place");
            CHECK(!strstr(ctor, "setenv(") && !strstr(ctor, "putenv("), "the constructor does not call setenv itself (no overwrite path)");
        }
    }
    printf("test-cienv: %d checks, %d failed\n", checks, fails); return fails ? 1 : 0;
}
