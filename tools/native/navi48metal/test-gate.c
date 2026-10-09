// test-gate.c: n48_gate.h (GPU-apps G4, kext 0.0.640: the bundle's load policy outside WindowServer) on the host, plus source pins that tie the REAL Navi48Device.m to it.
// Build/run on the host Mac (from this directory; argv[1] = the directory holding Navi48Device.m and n48_gate.h, default "."):
//   cc -O1 -Wall -Wextra -Werror -o /tmp/test-gate test-gate.c && /tmp/test-gate .
// tests/../test-gate-plant.sh plants breaks in the REAL n48_gate.h and Navi48Device.m and demands that this suite FAILS on each.
// Covers:
//   G1  process classes (WindowServer / N48M_ALLOW root / other);
//   G2  the kill file: ENOENT / ENOTDIR = absent, EPERM / EACCES / anything else = UNREADABLE; WindowServer keeps today's rule (only a present file declines), everyone else FAILS CLOSED;
//   G3  the nub flags: WindowServer declines only on Ready == 0; everyone else needs Ready present and 1; AutoDisarmed == 1 declines everywhere;
//   G4  the kernel probe: every step must succeed, HUNG declines, only ExclusiveAccess is retried (bounded);
//   G5  the pipeline-cache path: WindowServer / root keep /private/var/tmp (or /private/tmp, or the test directory); an application gets its own file under the user cache dir, none without it;
//   G6  the spvcache-miss fallback: now also for an admitted application, still not for an unadmitted process;
//   G7  constants agree with the kernel ABI header (type, selectors, info size / flags offset);
//   G8  source pins on Navi48Device.m: reachability and order of the gates, the probe last and OTHER-only, the admitted flag set only after the probe;
//   G9  (0.0.641, G4 review HIGH 2) the probe runs only for euid >= 501: root / daemons are declined by class, first, with 0.0.632's text and no kernel open;
//   G10 (0.0.641, G4 review LOW) the AIR dump directory: mkdir 0700, lstat, a real directory owned by the caller, never a chmod that follows a link.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <errno.h>
#include "n48_gate.h"
#include "../../../src/navi48-bringup/src/Navi48NativeABI.h"

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

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : ".";
    // ---- G1 ----
    CHECK("G1: WindowServer is class WS even with N48M_ALLOW", n48g_class(1, 0) == N48G_CLASS_WS && n48g_class(1, 1) == N48G_CLASS_WS);
    CHECK("G1: a root tool with N48M_ALLOW=1 is ROOT", n48g_class(0, 1) == N48G_CLASS_ROOT);
    CHECK("G1: anything else is OTHER", n48g_class(0, 0) == N48G_CLASS_OTHER);
    // ---- G2 ----
    CHECK("G2: stat ok -> PRESENT", n48g_kill_state(0, 0) == N48G_KILL_PRESENT && n48g_kill_state(0, EPERM) == N48G_KILL_PRESENT);
    CHECK("G2: ENOENT / ENOTDIR -> ABSENT", n48g_kill_state(-1, ENOENT) == N48G_KILL_ABSENT && n48g_kill_state(-1, ENOTDIR) == N48G_KILL_ABSENT);
    CHECK("G2: EPERM (the sandbox) -> UNREADABLE", n48g_kill_state(-1, EPERM) == N48G_KILL_UNREADABLE);
    CHECK("G2: EACCES / EIO / ELOOP -> UNREADABLE", n48g_kill_state(-1, EACCES) == N48G_KILL_UNREADABLE && n48g_kill_state(-1, EIO) == N48G_KILL_UNREADABLE && n48g_kill_state(-1, ELOOP) == N48G_KILL_UNREADABLE);
    for (int ws = 0; ws <= 1; ws++) {
        CHECK("G2: a PRESENT kill file declines everyone", n48g_kill_declines(ws, N48G_KILL_PRESENT, 0));
        CHECK("G2: a PRESENT kill file with the root test bypass does not decline", !n48g_kill_declines(ws, N48G_KILL_PRESENT, 1));
        CHECK("G2: an ABSENT kill file declines nobody", !n48g_kill_declines(ws, N48G_KILL_ABSENT, 0));
    }
    CHECK("G2: UNREADABLE does NOT decline WindowServer (today's rule, bit for bit)", !n48g_kill_declines(1, N48G_KILL_UNREADABLE, 0));
    CHECK("G2: UNREADABLE declines every other process (fail CLOSED)", n48g_kill_declines(0, N48G_KILL_UNREADABLE, 0));
    CHECK("G2: the bypass does not excuse an UNREADABLE file", n48g_kill_declines(0, N48G_KILL_UNREADABLE, 1));
    // ---- G3 ----
    CHECK("G3: WindowServer declines only on Ready == 0", n48g_ready_declines(1, 0) && !n48g_ready_declines(1, 1) && !n48g_ready_declines(1, N48G_FLAG_ABSENT) && !n48g_ready_declines(1, N48G_FLAG_NONUB));
    CHECK("G3: another process needs Ready present and 1", !n48g_ready_declines(0, 1) && n48g_ready_declines(0, 0) && n48g_ready_declines(0, N48G_FLAG_ABSENT) && n48g_ready_declines(0, N48G_FLAG_NONUB));
    CHECK("G3: AutoDisarmed == 1 declines everyone, absent / 0 declines nobody", n48g_autodisarm_declines(0, 1) && n48g_autodisarm_declines(1, 1) && !n48g_autodisarm_declines(0, 0) && !n48g_autodisarm_declines(0, N48G_FLAG_ABSENT) && !n48g_autodisarm_declines(1, N48G_FLAG_NONUB));
    // ---- G4 ----
    CHECK("G4: all steps ok, not HUNG -> OK", n48g_probe_verdict(1, 0, 0, 0, 0, 2u) == N48G_PROBE_OK);
    CHECK("G4: no service -> NOSVC", n48g_probe_verdict(0, 0, 0, 0, 0, 2u) == N48G_PROBE_NOSVC);
    CHECK("G4: a refused open -> OPEN (any code)", n48g_probe_verdict(1, 0xe00002c1u, 0, 0, 0, 2u) == N48G_PROBE_OPEN && n48g_probe_verdict(1, N48G_KR_EXCLUSIVE, 0, 0, 0, 2u) == N48G_PROBE_OPEN);
    CHECK("G4: a failed Hello -> HELLO", n48g_probe_verdict(1, 0, 0xe00002c7u, 0, 0, 2u) == N48G_PROBE_HELLO);
    CHECK("G4: an unreadable QueryInfo -> INFO", n48g_probe_verdict(1, 0, 0, 0xe00002c0u, 0, 2u) == N48G_PROBE_INFO);
    CHECK("G4: HUNG -> HUNG, other flag bits are fine", n48g_probe_verdict(1, 0, 0, 0, N48G_INFO_HUNG, 2u) == N48G_PROBE_HUNG && n48g_probe_verdict(1, 0, 0, 0, 6u, 3u) == N48G_PROBE_OK);
    CHECK("G4: only ExclusiveAccess is retried", n48g_probe_retry(N48G_KR_EXCLUSIVE, 0) && !n48g_probe_retry(0xe00002c1u, 0) && !n48g_probe_retry(0u, 0));
    CHECK("G4: the retry is bounded", n48g_probe_retry(N48G_KR_EXCLUSIVE, N48G_PROBE_RETRIES - 1) && !n48g_probe_retry(N48G_KR_EXCLUSIVE, N48G_PROBE_RETRIES));
    CHECK("G4: a root session (valid budget flags WITHOUT the APP bit, or none) is not an application", n48g_probe_verdict(1, 0, 0, 0, 0, 0u) == N48G_PROBE_NOTAPP && n48g_probe_verdict(1, 0, 0, 0, 0, 1u) == N48G_PROBE_NOTAPP);
    CHECK("G4: HUNG is judged before the role", n48g_probe_verdict(1, 0, 0, 0, N48G_INFO_HUNG, 0u) == N48G_PROBE_HUNG);
    { int ok = 1; for (int v = 0; v <= 6; v++) ok &= strcmp(n48g_probe_text(v), "?") != 0; CHECK("G4: every verdict has a text", ok); }
    // ---- G5 ----
    { char p[300];
      CHECK("G5: WindowServer: /private/var/tmp when writable", n48g_pcache_path(p, sizeof p, N48G_CLASS_WS, NULL, 1, "/var/folders/xx/C/", "WindowServer") && !strcmp(p, "/private/var/tmp/n48m-pipecache.bin"));
      CHECK("G5: WindowServer: /private/tmp when /private/var/tmp is not writable", n48g_pcache_path(p, sizeof p, N48G_CLASS_WS, NULL, 0, "/x/", "WindowServer") && !strcmp(p, "/private/tmp/n48m-pipecache.bin"));
      CHECK("G5: a root tool honours the test directory", n48g_pcache_path(p, sizeof p, N48G_CLASS_ROOT, "/tmp/t", 1, "/x/", "mtlprobe") && !strcmp(p, "/tmp/t/n48m-pipecache.bin"));
      CHECK("G5: an application gets its OWN file in the user cache dir", n48g_pcache_path(p, sizeof p, N48G_CLASS_OTHER, NULL, 1, "/var/folders/xx/C/", "mpv") && !strcmp(p, "/var/folders/xx/C/n48m-pipecache.mpv.bin"));
      CHECK("G5: ... even if /private/var/tmp is writable and a test dir is offered", n48g_pcache_path(p, sizeof p, N48G_CLASS_OTHER, "/tmp/t", 1, "/c/", "app") && !strcmp(p, "/c/n48m-pipecache.app.bin"));
      CHECK("G5: the cache dir without a trailing slash still works", n48g_pcache_path(p, sizeof p, N48G_CLASS_OTHER, NULL, 0, "/c", "app") && !strcmp(p, "/c/n48m-pipecache.app.bin"));
      CHECK("G5: an application with no user cache dir has NO cache (never /private/var/tmp)", n48g_pcache_path(p, sizeof p, N48G_CLASS_OTHER, NULL, 1, "", "mpv") == 0 && p[0] == 0 && n48g_pcache_path(p, sizeof p, N48G_CLASS_OTHER, NULL, 1, NULL, "mpv") == 0);
      CHECK("G5: path separators and odd characters in the program name are neutralised", n48g_pcache_path(p, sizeof p, N48G_CLASS_OTHER, NULL, 1, "/c/", "../a b/c") && !strcmp(p, "/c/n48m-pipecache..._a_b_c.bin") && strchr(p + 3, '/') == NULL);
      CHECK("G5: an empty program name becomes app", n48g_pcache_path(p, sizeof p, N48G_CLASS_OTHER, NULL, 1, "/c/", "") && !strcmp(p, "/c/n48m-pipecache.app.bin"));
      CHECK("G5: a long program name is cut to 40 characters", n48g_pcache_path(p, sizeof p, N48G_CLASS_OTHER, NULL, 1, "/c/", "0123456789012345678901234567890123456789ZZZZ") && !strcmp(p, "/c/n48m-pipecache.0123456789012345678901234567890123456789.bin"));
      CHECK("G5: a buffer too small -> no cache, empty path", n48g_pcache_path(p, 20, N48G_CLASS_WS, NULL, 1, "", "x") == 0 && p[0] == 0);
      CHECK("G5: who saves: WindowServer yes, root only with the switch, an application yes", n48g_pcache_save(N48G_CLASS_WS, 0) == 1 && n48g_pcache_save(N48G_CLASS_ROOT, 0) == 0 && n48g_pcache_save(N48G_CLASS_ROOT, 1) == 1 && n48g_pcache_save(N48G_CLASS_OTHER, 0) == 1);
    }
    // ---- G6 ----
    CHECK("G6: WindowServer / force / test hook get the fallback as before", n48g_fallback_ok(1, 0, 0, 0) && n48g_fallback_ok(0, 1, 0, 0) && n48g_fallback_ok(0, 0, 1, 0));
    CHECK("G6: an application the kernel admitted gets it too", n48g_fallback_ok(0, 0, 0, 1));
    CHECK("G6: an unadmitted process still gets the NSError (no fallback)", !n48g_fallback_ok(0, 0, 0, 0));
    // ---- G7 ----
    CHECK("G7: the open type is 'N48N'", N48G_UC_TYPE == N48N_UC_TYPE);
    CHECK("G7: Hello / QueryInfo selectors", N48G_SEL_HELLO == (unsigned)N48N_SEL_HELLO && N48G_SEL_QUERYINFO == (unsigned)N48N_SEL_QUERYINFO);
    CHECK("G7: Hello's minor flag", N48G_HELLO_F_MINOR == (unsigned long long)N48N_HELLO_F_MINOR);
    CHECK("G7: the info size and the flags offset", N48G_INFO_SIZE == sizeof(struct n48n_info) && N48G_INFO_FLAGS_OFF == offsetof(struct n48n_info, flags));
    CHECK("G7: the HUNG bit", N48G_INFO_HUNG == N48N_INFO_HUNG);
    CHECK("G7: the budget flags word is info.reserved[3] and the APP bit is the ABI's", N48G_INFO_BUDGET_OFF == offsetof(struct n48n_info, reserved) + 4u * N48N_INFO_R_BUDGET_FLAGS && N48G_BUDGET_APP == N48N_BUDGET_APP);
    CHECK("G7: ExclusiveAccess is the IOReturn value", N48G_KR_EXCLUSIVE == 0xe00002c5u);
    CHECK("G7: ABI minor is 12", N48N_ABI_MINOR == 12u);
    // ---- G8 ----
    char *m = slurp(dir, "Navi48Device.m");
    CHECK("G8: Navi48Device.m is readable", m != NULL);
    if (m) {
        const char *adm = fn_start(m, "static const char *n48_admit(uint32_t port) {");
        const char *adm_end = adm ? strstr(adm, "\nstatic BOOL n48_headless(void)") : NULL;
        CHECK("G8: n48_admit is found", adm && adm_end);
        if (adm && adm_end) {
            const char *kill = strstr(adm, "n48g_kill_declines(isWs, ks, killIgnored)"), *rdy = strstr(adm, "n48g_ready_declines(isWs, rdy)"), *ad = strstr(adm, "n48g_autodisarm_declines(isWs"),
                       *cnt = strstr(adm, "n48_counter_tripped(&count)"), *probe = strstr(adm, "n48_app_probe(port)"), *flag = strstr(adm, "atomic_store(&n48_app_admitted, 1)"), *cls = strstr(adm, "if (cls == N48G_CLASS_OTHER) {");
            CHECK("G8: every gate is in n48_admit", kill && rdy && ad && cnt && probe && flag && cls);
            CHECK("G8: order: kill file, Ready, AutoDisarmed, crash counter, then the kernel probe", before(kill, rdy) && before(rdy, ad) && before(ad, cnt) && before(cnt, probe));
            const char *whyc = strstr(adm, "if (why) return why;");
            CHECK("G8: the probe runs only for class OTHER, a refusal returns before the admitted flag, and the flag is set after the probe", whyc && before(cls, probe) && before(probe, whyc) && before(whyc, flag));
            CHECK("G8: the admitted flag is stored in exactly one place", strstr(m, "atomic_store(&n48_app_admitted, 1)") == strstr(adm, "atomic_store(&n48_app_admitted, 1)") && !strstr(strstr(m, "atomic_store(&n48_app_admitted, 1)") + 10, "atomic_store(&n48_app_admitted, 1)"));
            CHECK("G8: the old name-based refusal text is only reachable through the by-class decline (0.0.641: euid < 501)", !has(adm, adm_end, "process is not WindowServer and N48M_ALLOW=1 (as root) is not set") && has(adm, adm_end, "return N48G_DECLINE_BY_CLASS_TEXT;"));
            CHECK("G8: the plain stat()==0 test is gone (the kill file is read through n48g_kill_state)", !has(adm, adm_end, "if (stat(N48_KILL_FILE, &st) == 0)") && has(adm, adm_end, "n48g_kill_state(srcRc, srcErr)"));
            CHECK("G8: the Ready test is the gate function, not a bare == 0", !has(adm, adm_end, "n48_nub_ready(port) == 0") && !has(adm, adm_end, "n48_nub_autodisarmed(port) == 1"));
        }
        const char *probe = fn_start(m, "static const char *n48_app_probe(uint32_t port) {");
        const char *probe_end = probe ? strstr(probe, "\n// NULL = admit, else the reason") : NULL;
        CHECK("G8: n48_app_probe is found", probe && probe_end);
        if (probe && probe_end) {
            const char *o = strstr(probe, "IOServiceOpen(svc, mach_task_self(), N48G_UC_TYPE, &conn)"), *h = strstr(probe, "N48G_SEL_HELLO"), *q = strstr(probe, "N48G_SEL_QUERYINFO"), *c = strstr(probe, "IOServiceClose(conn)"), *v = strstr(probe, "n48g_probe_verdict(");
            CHECK("G8: the probe opens, says Hello, reads QueryInfo, closes, then judges", o && h && q && c && v && before(o, h) && before(h, q) && before(q, c) && before(c, v));
            CHECK("G8: the probe reads the APP role from the kernel's QueryInfo (never assumes it)", has(probe, probe_end, "memcpy(&budgetFlags, info + N48G_INFO_BUDGET_OFF, sizeof budgetFlags);") && !has(probe, probe_end, "budgetFlags = 2") && !has(probe, probe_end, "budgetFlags |="));
            CHECK("G8: the probe never calls Vulkan or the lazy RADV open", !has(probe, probe_end, "n48_radv_open") && !has(probe, probe_end, "vkCreate"));
            CHECK("G8: the verdict comes from the pure function, the probe returns NULL only on OK", strstr(probe, "return v == N48G_PROBE_OK ? NULL : n48g_probe_text(v);") != NULL);
        }
        CHECK("G8: n48_fallback_ok asks the pure function with the admitted flag", strstr(m, "n48g_fallback_ok(n48_is_ws(), n48_force_fallback(), n48_test_fb_as_ws(), atomic_load(&n48_app_admitted))") != NULL);
        const char *pc = fn_start(m, "static void n48_pc_open(const VkPhysicalDeviceProperties *pp) {");
        const char *pc_end = pc ? strstr(pc, "static void n48_pc_save(") : NULL;
        CHECK("G8: n48_pc_open is found", pc && pc_end);
        if (pc && pc_end) {
            const char *conf = strstr(pc, "confstr(_CS_DARWIN_USER_CACHE_DIR"), *path = strstr(pc, "n48g_pcache_path(N48PC.path, sizeof N48PC.path, pcls, envdir, vtw, ucd, getprogname())"), *sv = strstr(pc, "n48g_pcache_save(pcls");
            CHECK("G8: the cache path comes from the pure function after reading the user cache dir", conf && path && sv && before(conf, path) && before(path, sv));
            CHECK("G8: the cache class is the process class", has(pc, pc_end, "const int pcls = n48g_class(n48_is_ws(), n48_allow());"));
            CHECK("G8: the cache directory is no longer hard-coded in n48_pc_open", !has(pc, pc_end, "snprintf(N48PC.path, sizeof N48PC.path, \"%s/n48m-pipecache.bin\", dir)"));
        }
        CHECK("G8: n48_nub_flag tells an unreachable nub (-2) from an absent property (-1)", strstr(m, "kIOServicePlane, &parent) != KERN_SUCCESS || !parent) return N48G_FLAG_NONUB;") != NULL && strstr(m, "if (!v) return N48G_FLAG_ABSENT;") != NULL);
        free(m);
    }
    // ---- G9 ----
    CHECK("G9: the by-class text is 0.0.632's refusal, word for word", !strcmp(N48G_DECLINE_BY_CLASS_TEXT, "process is not WindowServer and N48M_ALLOW=1 (as root) is not set"));
    CHECK("G9: the bound is 501, the kernel's kAppMinUid", N48G_APP_MIN_EUID == 501u);
    { static const uint32_t euids[] = { 0, 1, 88, 89, 200, 499, 500, 501, 502, 1000, 65534, 0xFFFFFFFFu };
      int declined = 0, probed = 0;
      for (unsigned i = 0; i < sizeof euids / sizeof euids[0]; i++) for (int ws = 0; ws <= 1; ws++) for (int allow = 0; allow <= 1; allow++) {
          const int cls = n48g_class(ws, allow), d = n48g_declined_by_class(cls, euids[i]);
          const int probe = (cls == N48G_CLASS_OTHER) && !d;   // the flow of n48_admit: the probe is reached only for class OTHER that was not declined first
          if (cls != N48G_CLASS_OTHER) CHECK("G9: WindowServer and N48M_ALLOW root tools are never declined by class, whatever their euid", !d && !probe);
          else if (euids[i] < 501u) { declined++; CHECK("G9: class OTHER with euid < 501 (root, daemons) is declined by class and never probed", d && !probe); }
          else { probed++; CHECK("G9: class OTHER with euid >= 501 is not declined by class and IS probed (reachable)", !d && probe); }
      }
      CHECK("G9: both branches were reached", declined > 0 && probed > 0); }
    char *m2 = slurp(dir, "Navi48Device.m");
    CHECK("G9: Navi48Device.m is readable", m2 != NULL);
    if (m2) {
        const char *adm = fn_start(m2, "static const char *n48_admit(uint32_t port) {");
        const char *adm_end = adm ? strstr(adm, "\nstatic BOOL n48_headless(void)") : NULL;
        if (adm && adm_end) {
            const char *dec = strstr(adm, "if (n48g_declined_by_class(cls, (uint32_t)geteuid())) return N48G_DECLINE_BY_CLASS_TEXT;"), *stt = strstr(adm, "stat(N48_KILL_FILE, &st)"), *pr = strstr(adm, "n48_app_probe(port)"), *gate = strstr(adm, "if (cls == N48G_CLASS_OTHER) {");
            CHECK("G9: n48_admit declines by class FIRST: before the kill file, before any nub read, before the probe", dec && stt && pr && gate && before(dec, stt) && before(dec, pr) && before(dec, gate));
            CHECK("G9: the euid used is geteuid(), not getuid()", dec && !has(adm, adm_end, "getuid()"));
        }
    }
    // ---- G10 (0.0.641 rules, kept; build 6: per-uid subdirectories) ----
    CHECK("G10: a real directory owned by the caller is usable", n48g_dumpdir_ok(0, 1, 0, 501, 501) && n48g_dumpdir_ok(0, 1, 0, 0, 0) && n48g_dumpdir_ok(0, 1, 0, 88, 88));
    CHECK("G10: a symlink is refused (even to a directory of ours)", !n48g_dumpdir_ok(0, 0, 1, 501, 501) && !n48g_dumpdir_ok(0, 1, 1, 501, 501));
    CHECK("G10: a directory owned by someone else is refused (a planted one)", !n48g_dumpdir_ok(0, 1, 0, 88, 501) && !n48g_dumpdir_ok(0, 1, 0, 501, 0) && !n48g_dumpdir_ok(0, 1, 0, 0, 501));
    CHECK("G10: a failed lstat or a plain file is refused", !n48g_dumpdir_ok(-1, 1, 0, 501, 501) && !n48g_dumpdir_ok(0, 0, 0, 501, 501));
    CHECK("G10: any group / other bit is narrowed, 0700 is left alone", n48g_dumpdir_needs_narrow(0777) && n48g_dumpdir_needs_narrow(0770) && n48g_dumpdir_needs_narrow(0707) && n48g_dumpdir_needs_narrow(0755) && n48g_dumpdir_needs_narrow(01777) && !n48g_dumpdir_needs_narrow(0700) && !n48g_dumpdir_needs_narrow(0500));
    CHECK("G10: the parent is /tmp/n48m", !strcmp(N48G_DUMP_DIR, "/tmp/n48m"));
    // ---- G12 (build 6, C1 finding A): the parent (sticky 01777) and the per-uid subdirectory ----
    CHECK("G12: parent: a real sticky-1777 directory owned by root, by the caller or by the daemon user is usable", n48g_dumproot_ok(0, 1, 0, 0, 041777, 501) && n48g_dumproot_ok(0, 1, 0, 501, 041777, 501) && n48g_dumproot_ok(0, 1, 0, 88, 01777, 88) && n48g_dumproot_ok(0, 1, 0, N48G_DAEMON_UID, 01777, 501));
    CHECK("G12: parent: a symlink, a failed lstat or a plain file is refused", !n48g_dumproot_ok(0, 0, 1, 0, 01777, 501) && !n48g_dumproot_ok(-1, 1, 0, 0, 01777, 501) && !n48g_dumproot_ok(0, 0, 0, 0, 01777, 501));
    CHECK("G12: parent: owned by a THIRD user is refused even at 1777 (a planted one)", !n48g_dumproot_ok(0, 1, 0, 502, 01777, 501) && !n48g_dumproot_ok(0, 1, 0, 88, 01777, 501));
    CHECK("G12: parent: without the sticky bit or world-write it is refused (0700 from a pre-build-6 WindowServer, 0777 without sticky, 0755)", !n48g_dumproot_ok(0, 1, 0, 88, 0700, 88) && !n48g_dumproot_ok(0, 1, 0, 0, 0777, 501) && !n48g_dumproot_ok(0, 1, 0, 0, 0755, 501) && !n48g_dumproot_ok(0, 1, 0, 0, 01755, 501));
    CHECK("G12: parent: extra setuid / setgid bits are refused", !n48g_dumproot_ok(0, 1, 0, 0, 03777, 501) && !n48g_dumproot_ok(0, 1, 0, 0, 05777, 501));
    CHECK("G12: parent repair: only a parent that is OURS and not 1777 is repaired", n48g_dumproot_needs_fix(88, 88, 0700) && n48g_dumproot_needs_fix(501, 501, 01755) && !n48g_dumproot_needs_fix(501, 501, 01777) && !n48g_dumproot_needs_fix(0, 501, 0700) && !n48g_dumproot_needs_fix(88, 501, 0700));
    { char sp[64];
      CHECK("G12: the subdirectory is /tmp/n48m/<euid>", n48g_dumpsub_path(sp, sizeof sp, 501) && !strcmp(sp, "/tmp/n48m/501"));
      CHECK("G12: WindowServer (88) keeps ITS OWN subdirectory", n48g_dumpsub_path(sp, sizeof sp, 88) && !strcmp(sp, "/tmp/n48m/88"));
      CHECK("G12: distinct uids give distinct subdirectories, uid 0 included, the largest uid fits", n48g_dumpsub_path(sp, sizeof sp, 0) && !strcmp(sp, "/tmp/n48m/0") && n48g_dumpsub_path(sp, sizeof sp, 4294967294u) && !strcmp(sp, "/tmp/n48m/4294967294"));
      CHECK("G12: a too-small buffer yields 0 and an empty string", n48g_dumpsub_path(sp, 8, 501) == 0 && sp[0] == 0); }
    CHECK("G12: the daemon user is nobody (uid -2)", N48G_DAEMON_UID == 4294967294u);
    if (m2) {
        const char *rd = fn_start(m2, "static int n48_dump_dir_ready(char *sub, size_t subn) {");
        const char *rd_end = rd ? strstr(rd, "\n// A dump file appears under its final name only when complete") : NULL;
        CHECK("G12: n48_dump_dir_ready is found", rd && rd_end);
        if (rd && rd_end) {
            const char *mkr = strstr(rd, "mkdir(N48G_DUMP_DIR, N48G_DUMP_ROOT_MODE)"), *lr = strstr(rd, "lstat(N48G_DUMP_DIR, &rs)"), *fx = strstr(rd, "n48g_dumproot_needs_fix("), *fcr = strstr(rd, "fchmodat(AT_FDCWD, N48G_DUMP_DIR, N48G_DUMP_ROOT_MODE, AT_SYMLINK_NOFOLLOW)"),
                       *okr = strstr(rd, "if (!n48g_dumproot_ok(rrc, "), *sp = strstr(rd, "n48g_dumpsub_path(sub, subn, (uint32_t)geteuid())"), *mks = strstr(rd, "mkdir(sub, 0700)"), *ls = strstr(rd, "lstat(sub, &st)"),
                       *ok = strstr(rd, "if (!n48g_dumpdir_ok(rc, S_ISDIR(st.st_mode), S_ISLNK(st.st_mode), (uint32_t)st.st_uid, (uint32_t)geteuid())"), *op = strstr(rd, "open(sub, O_RDONLY | O_DIRECTORY | O_NOFOLLOW)"),
                       *nr = strstr(rd, "n48g_dumpdir_needs_narrow((uint32_t)fs.st_mode)"), *fm = strstr(rd, "fchmod(fd, 0700)"), *ac = strstr(rd, "n48da_grant_list(fd, (uid_t)N48G_DAEMON_UID)");
            CHECK("G12: parent: mkdir, lstat, repair (no-follow), re-lstat verdict, THEN the subdirectory", mkr && lr && fx && fcr && okr && sp && before(mkr, lr) && before(lr, fx) && before(fx, fcr) && before(fcr, okr) && before(okr, sp));
            CHECK("G12: subdirectory: mkdir 0700, lstat, the ownership / symlink verdict, an O_NOFOLLOW open, narrowing and the ACL grant on that descriptor (in this order)", mks && ls && ok && op && nr && fm && ac && before(sp, mks) && before(mks, ls) && before(ls, ok) && before(ok, op) && before(op, nr) && before(nr, fm) && before(fm, ac));
            CHECK("G12: both refusals return 0 BEFORE any narrowing, ACL or write", has(rd, rd_end, "return 0;") && before(strstr(rd, "return 0;"), op) && before(okr, strstr(okr, "return 0;")) );
            CHECK("G12: the descriptor must be the directory lstat described (inode, device, owner)", has(rd, rd_end, "fs.st_ino != st.st_ino") && has(rd, rd_end, "fs.st_dev != st.st_dev") && has(rd, rd_end, "(uint32_t)fs.st_uid != (uint32_t)geteuid()"));
            CHECK("G12: no stat() (it follows links), no plain chmod, and no path-based ACL call in the helper", !has(rd, rd_end, " stat(N48G_DUMP_DIR") && !has(rd, rd_end, " stat(sub") && !has(rd, rd_end, " chmod(") && !has(rd, rd_end, "acl_set_file") && !has(rd, rd_end, "acl_set_link_np"));
            CHECK("G12: the 0.0.641 narrowing of the parent is gone (the parent MUST stay sticky 1777)", !has(rd, rd_end, "fchmodat(AT_FDCWD, N48G_DUMP_DIR, 0700"));
        }
        const char *df = fn_start(m2, "static void n48_dump_function(");
        const char *df_end = df ? strstr(df, "\nstatic NSError *n48_dump_pipeline(") : NULL;
        CHECK("G10: n48_dump_function is found", df && df_end);
        if (df && df_end) {
            const char *rdy = strstr(df, "if (!n48_dump_dir_ready(sub, sizeof sub)) {"), *sd = strstr(df, "n48_dump_write(side.UTF8String, js)"), *wr = strstr(df, "n48_dump_write(air.UTF8String, bc)");
            CHECK("G10: REACHABLE: both dump files are written only after the directory check passed", rdy && sd && wr && before(rdy, sd) && has(df, df_end, "return; }"));
            CHECK("G12: the sidecar is written BEFORE the .air (the daemon starts on the .air)", sd && wr && before(sd, wr));
            CHECK("G12: dump paths are under the per-uid subdirectory, not the flat parent", has(df, df_end, "@\"%s/%@.air\", sub, sha") && has(df, df_end, "@\"%s/%@.%s.json\", sub, sha, role") && !has(df, df_end, "/tmp/n48m/%@"));
        }
        const char *dw = fn_start(m2, "static int n48_dump_write(const char *path, NSData *data) {");
        const char *dw_end = dw ? strstr(dw, "\n// Dumps one function's bitcodeData") : NULL;
        CHECK("G12: n48_dump_write is found", dw && dw_end);
        if (dw && dw_end) {
            const char *op = strstr(dw, "O_NOFOLLOW, 0600"), *fc = strstr(dw, "fchmod(fd, 0644)"), *rn = strstr(dw, "rename(tmp, path)");
            CHECK("G12: a file appears under its final name only complete and 0644: O_NOFOLLOW temp, fchmod 0644, rename", op && fc && rn && before(op, fc) && before(fc, rn));
            CHECK("G12: the temp name does not end in .air / .json (the daemon only looks at *.air)", has(dw, dw_end, "\"%s.tmp%d\""));
        }
        CHECK("G10: the 0777 mkdir and chmod are gone from the whole bundle", !strstr(m2, "mkdir(\"/tmp/n48m\", 0777)") && !strstr(m2, "chmod(\"/tmp/n48m\", 0777)"));
        CHECK("G10: no plain chmod of the dump directories anywhere", !strstr(m2, "chmod(\"/tmp/n48m") && !strstr(m2, "chmod(sub"));
    }
    // ---- G13 (build 6, C1): the bounded synchronous wait ----
    CHECK("G13: only an ADMITTED application waits (never WindowServer, never a root switch)", n48g_syncwait_applies(0, 1, 0, 0) && !n48g_syncwait_applies(1, 1, 0, 0) && !n48g_syncwait_applies(0, 0, 0, 0) && !n48g_syncwait_applies(0, 1, 1, 0) && !n48g_syncwait_applies(0, 1, 0, 1) && !n48g_syncwait_applies(1, 0, 0, 0));
    { int total = 0, el = 0, steps = 0, r;
      while ((r = n48g_sync_step(el, 0)) > 0) { total += r; el += r; steps++; if (steps > 10000) break; }
      CHECK("G13: a miss that never arrives stops after exactly the 3000 ms budget", r == 0 && total == N48G_SYNC_WAIT_MS && steps > 10 && steps < 200);
      CHECK("G13: each sleep is at most the poll interval and never overshoots the deadline", n48g_sync_step(0, 0) == N48G_SYNC_POLL_MS && n48g_sync_step(2990, 0) == 10 && n48g_sync_step(2999, 0) == 1 && n48g_sync_step(3000, 0) == 0 && n48g_sync_step(9999, 0) == 0);
      CHECK("G13: a hit stops the wait at once, at any time, even at the deadline", n48g_sync_step(0, 1) == -1 && n48g_sync_step(1500, 1) == -1 && n48g_sync_step(3000, 1) == -1);
      // simulated daemon: the file appears at t = 1230 ms
      el = 0; r = 0; int found_at = -1; while (1) { const int found = el >= 1230; r = n48g_sync_step(el, found); if (r == -1) { found_at = el; break; } if (r == 0) break; el += r; }
      CHECK("G13: a translation that arrives at 1230 ms is picked up within one poll interval", found_at >= 1230 && found_at < 1230 + N48G_SYNC_POLL_MS); }
    { n48g_sync st = {0};
      CHECK("G13: a fresh process waits", n48g_sync_allowed(&st));
      n48g_sync_result(&st, 0); n48g_sync_result(&st, 0);
      CHECK("G13: two timeouts still wait", n48g_sync_allowed(&st));
      n48g_sync_result(&st, 0);
      CHECK("G13: the third consecutive timeout switches the wait OFF (a dead daemon costs 9 s, not 3 s per pipeline)", !n48g_sync_allowed(&st));
      n48g_sync_result(&st, 1);
      CHECK("G13: a hit re-arms it", n48g_sync_allowed(&st) && st.strikes == 0);
      n48g_sync_result(&st, 0); n48g_sync_result(&st, 1); n48g_sync_result(&st, 0); n48g_sync_result(&st, 0);
      CHECK("G13: strikes count CONSECUTIVE timeouts only", n48g_sync_allowed(&st) && st.strikes == 2); }
    CHECK("G13: budget constants: 3000 ms wait, 50 ms poll, 3 strikes", N48G_SYNC_WAIT_MS == 3000 && N48G_SYNC_POLL_MS == 50 && N48G_SYNC_STRIKES == 3);
    if (m2) {
        const char *sw = fn_start(m2, "static BOOL n48_sync_wait(const char *what, NSArray<NSString *> *shas, BOOL needMeta) {");
        const char *sw_end = sw ? strstr(sw, "\n// ---------------------------------------------------------------------------------------------------------------\n// N48RenderPipelineState (10d/11e)") : NULL;
        CHECK("G13: n48_sync_wait is found", sw && sw_end);
        if (sw && sw_end) {
            CHECK("G13: the wait is gated by the pure rule with the admitted flag (WindowServer returns at once)", has(sw, sw_end, "n48g_syncwait_applies(n48_is_ws(), atomic_load(&n48_app_admitted), n48_force_fallback(), n48_test_fb_as_ws())"));
            CHECK("G13: the loop is driven by n48g_sync_step and the strike / timed-out bookkeeping by n48g_sync_result", has(sw, sw_end, "n48g_sync_step(el, found)") && has(sw, sw_end, "n48g_sync_result(&n48_sw_state, found ? 1 : 0)") && has(sw, sw_end, "n48g_sync_allowed(&n48_sw_state)"));
            CHECK("G13: the timed-out set is keyed by the function hashes of the pipeline", has(sw, sw_end, "NSString *key = [shas componentsJoinedByString:@\"+\"];") && has(sw, sw_end, "[n48_sw_timedout containsObject:key]") && has(sw, sw_end, "[n48_sw_timedout addObject:key]"));
            CHECK("G13: the wait looks at the lookup directories (n48hs_stat), spv with the sanity check", has(sw, sw_end, "n48hs_stat(sha, \".spv\", YES)"));
        }
        const char *ri = fn_start(m2, "- (instancetype)initWithDevice:(id)dev descriptor:(MTLRenderPipelineDescriptor *)d noFallback:(BOOL)nofb error:(NSError **)err {");
        const char *ri_end = ri ? strstr(ri, "N48sRefl *rv = NULL, *rf = NULL;") : NULL;
        CHECK("G13: the render miss branch is found", ri && ri_end);
        if (ri && ri_end) {
            const char *dm = strstr(ri, "(void)n48_dump_pipeline(d);"), *wt = strstr(ri, "n48_sync_wait(\"render\", want, NO)"), *fbk = strstr(ri, "fb = YES; _fallback = YES;"), *rl = strstr(ri, "vsd = n48_spv_lookup(d.vertexFunction, \"vertex\", &e1, &vname, &mv_);\n                    fsd");
            CHECK("G13: render: dump FIRST, then the wait, then (only if still missing) the fallback", dm && wt && fbk && before(dm, wt) && before(wt, fbk));
            CHECK("G13: render: after a successful wait both stages are looked up again before deciding", rl && before(wt, rl) && before(rl, fbk));
            CHECK("G13: render: WindowServer never reaches the wait (the branch is guarded by the pure rule)", has(ri, ri_end, "if (!inproc && n48g_syncwait_applies(n48_is_ws(), atomic_load(&n48_app_admitted), n48_force_fallback(), n48_test_fb_as_ws())) {"));
        }
        const char *cp = fn_start(m2, "- (id)n48NewComputePipeline:(id)fn error:(NSError **)error {");
        const char *cp_end = cp ? strstr(cp, "- (id)newComputePipelineStateWithFunction:(id)fn error:") : NULL;
        CHECK("G13: the compute miss branch is found", cp && cp_end);
        if (cp && cp_end) {
            const char *gd = strstr(cp, "if (!inproc && e.code == 41 && n48_fallback_ok() && n48g_syncwait_applies("), *dm = strstr(cp, "n48_dump_function(fn, \"kernel\", dp, dn);"), *wt = strstr(cp, "n48_sync_wait(\"kernel\", @[ ksha ], YES)"), *rt = strstr(cp, "initWithDevice:self function:fn error:&e2"), *ph = strstr(cp, "initPlaceholderWithDevice:self function:fn");
            CHECK("G13: compute: dump, wait (spv + meta), retry the real pipeline, THEN the no-op placeholder", gd && dm && wt && rt && ph && before(gd, dm) && before(dm, wt) && before(wt, rt) && before(rt, ph));
        }
    }
    // ---- G14 (build 6, C1 finding B): the GPU family claim ----
    CHECK("G14: Metal3 (5001) is NOT claimed for apps, kept for WindowServer", !n48g_supports_family(N48G_FAMILY_METAL3, 0) && n48g_supports_family(N48G_FAMILY_METAL3, 1) && N48G_FAMILY_METAL3 == 5001);
    CHECK("G14: Mac2 and Common1..3 are kept", n48g_supports_family(2002, 0) && n48g_supports_family(3001, 0) && n48g_supports_family(3002, 0) && n48g_supports_family(3003, 0) && n48g_supports_family(3001, 1));
    { int any = 0; static const long no[] = { 1001, 1002, 1003, 1004, 1005, 1006, 1007, 1008, 1009, 2001, 4001, 5001, 5002, 6001, 0, -1, 99999 };
      for (unsigned i = 0; i < sizeof no / sizeof no[0]; i++) any |= n48g_supports_family(no[i], 0) | (no[i] != N48G_FAMILY_METAL3 ? n48g_supports_family(no[i], 1) : 0);
      CHECK("G14: Apple1..9, Mac1, MacCatalyst, Metal3, Metal4 and unknown values are all NO", !any); }
    CHECK("G14: argument buffers report the lowest tier", N48G_ARGBUF_TIER1 == 0);
    if (m2) {
        const char *sf = fn_start(m2, "- (BOOL)supportsFamily:(NSInteger)family {");
        const char *sf_end = sf ? strstr(sf, "- (NSUInteger)argumentBuffersSupport") : NULL;
        CHECK("G14: supportsFamily asks the pure table (no case list in the .m)", sf && sf_end && has(sf, sf_end, "return n48g_supports_family((long)family, n48_is_ws() ? 1 : 0) ? YES : NO;") && !has(sf, sf_end, "case "));
        CHECK("G14: it logs once when Metal3 is asked for", sf && sf_end && has(sf, sf_end, "N48_ONCE(\"supportsFamily(Metal3) -> NO"));
        CHECK("G14: Metal3 is logged only for applications (WindowServer keeps YES)", sf && sf_end && has(sf, sf_end, "if (family == N48G_FAMILY_METAL3 && !n48_is_ws())"));
        CHECK("G14: WindowServer gets the base class's argumentBuffersSupport", strstr(m2, "return ((NSUInteger (*)(struct objc_super *, SEL))objc_msgSendSuper)(&sup, @selector(argumentBuffersSupport));") != NULL && strstr(m2, "    if (n48_is_ws()) {   // the base class") != NULL);
        CHECK("G14: argumentBuffersSupport is overridden and returns the Tier1 constant", strstr(m2, "- (NSUInteger)argumentBuffersSupport {") != NULL && strstr(m2, "return (NSUInteger)N48G_ARGBUF_TIER1; }") != NULL);
        CHECK("G14: the old Metal3 constant is gone from the .m", !strstr(m2, "N48_Metal3"));
    }
    free(m2);
    // ---- G15 (bundle 8, G6): sandboxed applications open N48N on the accelerator port the bundle holds ----
    CHECK("G15: WindowServer keeps Mesa's default service (no setter call)", n48g_mesa_service_for_class(N48G_CLASS_WS) == NULL);
    CHECK("G15: an N48M_ALLOW root tool keeps Mesa's default service", n48g_mesa_service_for_class(N48G_CLASS_ROOT) == NULL);
    CHECK("G15: an application (CLASS_OTHER) opens on Navi48Accelerator", n48g_mesa_service_for_class(N48G_CLASS_OTHER) && !strcmp(n48g_mesa_service_for_class(N48G_CLASS_OTHER), "Navi48Accelerator"));
    CHECK("G15: no other class value selects the accelerator", n48g_mesa_service_for_class(-1) == NULL && n48g_mesa_service_for_class(3) == NULL && n48g_mesa_service_for_class(99) == NULL);
    CHECK("G15: the setter's export name", !strcmp(N48G_MESA_SETTER, "radv_darwin_set_service_class"));
    { char *m5 = slurp(dir, "Navi48Device.m");
      CHECK("G15: Navi48Device.m is readable", m5 != NULL);
      if (m5) {
        const char *probe = fn_start(m5, "static const char *n48_app_probe(uint32_t port) {");
        const char *probe_end = probe ? strstr(probe, "\n// NULL = admit, else the reason") : NULL;
        CHECK("G15: the probe is found", probe && probe_end);
        if (probe && probe_end) {
            CHECK("G15: the probe opens on the port it was given (the accelerator), cast to io_service_t", has(probe, probe_end, "io_service_t svc = (io_service_t)port;"));
            CHECK("G15: the probe does not look a service up by name (no Navi48Bringup, no matching)", !has(probe, probe_end, "IOServiceMatching") && !has(probe, probe_end, "IOServiceGetMatchingService") && !has(probe, probe_end, "Navi48Bringup"));
            CHECK("G15: the probe does not release the caller's port", !has(probe, probe_end, "IOObjectRelease"));
            CHECK("G15: no port = no service for the pure verdict", has(probe, probe_end, "n48g_probe_verdict(svc != 0,"));
            CHECK("G15: the probe still opens type N48N", has(probe, probe_end, "IOServiceOpen(svc, mach_task_self(), N48G_UC_TYPE, &conn)"));
        }
        const char *ro = fn_start(m5, "static BOOL n48_radv_open_once(NSError **err) {");
        const char *ro_end = ro ? strstr(ro, "VkResult r = n48_vkCreateInstance(&ici, NULL, &N48R.inst);") : NULL;
        CHECK("G15: the RADV open (up to vkCreateInstance) is found", ro && ro_end);
        if (ro && ro_end) {
            const char *gp = strstr(ro, "n48_gipa = (PFN_vkGetInstanceProcAddr)dlsym(h, \"vk_icdGetInstanceProcAddr\");"), *sel = strstr(ro, "n48g_mesa_service_for_class(n48g_class(n48_is_ws(), n48_allow()))"), *gd = strstr(ro, "if (svcCls) {"),
                       *ds = strstr(ro, "dlsym(h, N48G_MESA_SETTER)"), *nos = strstr(ro, "if (!setSvc) {"), *call = strstr(ro, "setSvc(svcCls)"), *bad = strstr(ro, "if (src != 0) {");
            CHECK("G15: the route is chosen per class, after dlopen, and BEFORE vkCreateInstance", gp && sel && gd && ds && call && before(gp, sel) && before(sel, gd) && before(gd, ds) && before(ds, call));
            CHECK("G15: a missing setter or a refused name fails closed (error 7, goto fail) before any instance exists", nos && bad && before(nos, call) && before(call, bad) && has(nos, bad, "n48_err(7,") && has(bad, ro_end, "n48_err(7,") && has(nos, call, "goto fail;"));
            CHECK("G15: the setter is called in exactly one place", strstr(ro, "setSvc(svcCls)") == strstr(m5, "setSvc(svcCls)") && !strstr(strstr(m5, "setSvc(svcCls)") + 10, "setSvc("));
            CHECK("G15: WindowServer / root never reach the setter (the call is under `if (svcCls)`)", gd && call && before(gd, call) && !has(ro, gd, "if (n48_is_ws()") );
        }
        free(m5); } }
    // ---- G11 (0.0.641): the bundle build ----
    { char *pl = slurp(dir, "Info.plist");
      CHECK("G11: Info.plist: bundle version 0.1.1, build 20", pl && strstr(pl, "<key>CFBundleShortVersionString</key>\n\t<string>0.1.1</string>") && strstr(pl, "<key>CFBundleVersion</key>\n\t<string>20</string>"));
      free(pl); }
    printf("test-gate: %d checks, %d failed\n", runs, fails);
    return fails ? 1 : 0;
}
