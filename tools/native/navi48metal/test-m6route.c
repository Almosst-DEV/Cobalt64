// test-m6route.c: bundle build 11 (M6 Stage 1a): n48_m6route.h on the host - the kernel's IOSurface ID -> display table, the encode-time gate, the completion-time decision, the property parser - plus
// source pins on the glue in Navi48Device.m (the ObjC cannot run on the host; the decisions it calls are exercised below, the pins prove it calls them in the right order).
// Build/run on the host Mac:  cc -O1 -D_GNU_SOURCE -Wall -Wextra -Werror -o /tmp/test-m6route test-m6route.c && /tmp/test-m6route .      (the argument is tools/native/navi48metal)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "n48_scanabi.h"
#include "n48_m6route.h"
static int fails, checks;
#define CHECK(name, cond) do { checks++; if (!(cond)) { fails++; printf("FAIL: %s\n", name); } } while (0)
static char *slurp(const char *dir, const char *rel) {
    char path[1024]; snprintf(path, sizeof path, "%s/%s", dir, rel);
    FILE *f = fopen(path, "rb"); if (!f) { printf("FAIL: cannot read %s\n", path); fails++; return strdup(""); }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char *b = malloc((size_t)n + 1); size_t r = fread(b, 1, (size_t)n, f); b[r] = 0; fclose(f); return b;
}
static size_t countof(const char *h, const char *n) { size_t c = 0; const char *p = h; while ((p = strstr(p, n))) { c++; p += strlen(n); } return c; }
static void w32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
// a property blob exactly as src/navi48-bringup/src/amd/native_m6_pure.h blob_build lays it out
static size_t blob(uint8_t *out, uint32_t n, const uint32_t (*ents)[3]) {
    w32(out, N48M6_BLOB_VERSION); w32(out + 4, n);
    for (uint32_t i = 0; i < n; i++) { w32(out + 8 + i * 12, ents[i][0]); w32(out + 12 + i * 12, ents[i][1]); w32(out + 16 + i * 12, ents[i][2]); }
    return 8 + (size_t)n * 12;
}
int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : ".";
    uint8_t b[512]; n48m6_t s; memset(&s, 0, sizeof s);
    // ---- the property parser
    { const uint32_t e[4][3] = { { 101, 0, 0 }, { 201, 2, 0 }, { 202, 2, 0 }, { 301, 0, N48M6_ENT_AMBIGUOUS } };
      size_t n = blob(b, 4, e);
      CHECK("parse: a good table", n48m6_parse(&s, b, n) == 0 && s.n == 4 && s.have_table && s.e[1].id == 201 && s.e[1].inst == 2 && s.e[3].flags == N48M6_ENT_AMBIGUOUS);
      CHECK("counts per instance", n48m6_count_inst(&s, 0) == 2 && n48m6_count_inst(&s, 2) == 2 && n48m6_count_inst(&s, 1) == 0 && n48m6_count_ambig(&s) == 1);
      n48m6_t keep = s;
      CHECK("parse: a short blob is refused", n48m6_parse(&s, b, 4) == -1);
      CHECK("parse: a trailing byte is refused", n48m6_parse(&s, b, n + 1) == -1);
      CHECK("parse: a missing entry is refused", n48m6_parse(&s, b, n - 12) == -1);
      { uint8_t c[512]; memcpy(c, b, n); w32(c, 3); CHECK("parse: another version is refused", n48m6_parse(&s, c, n) == -1); }
      { uint8_t c[512]; memcpy(c, b, n); w32(c + 4, N48M6_MAX_SURF + 1); CHECK("parse: a count above the table is refused", n48m6_parse(&s, c, n) == -1); }
      { uint8_t c[512]; memcpy(c, b, n); w32(c + 8, 0); CHECK("parse: an entry with ID 0 rejects the whole table", n48m6_parse(&s, c, n) == -1); }
      { uint8_t c[512]; memcpy(c, b, n); w32(c + 12 + 12, 3); CHECK("parse: an entry on instance 3 rejects the whole table", n48m6_parse(&s, c, n) == -1); }
      { uint32_t big[N48M6_MAX_SURF + 1][3]; for (uint32_t i = 0; i < N48M6_MAX_SURF + 1; i++) { big[i][0] = 1000 + i; big[i][1] = 0; big[i][2] = 0; }
        uint8_t c[8 + (N48M6_MAX_SURF + 1) * 12]; size_t m = blob(c, N48M6_MAX_SURF + 1, (const uint32_t (*)[3])big);
        CHECK("parse: a WELL-FORMED blob of 17 entries (length matches) is refused by the count alone", n48m6_parse(&s, c, m) == -1);
        size_t m16 = blob(c, N48M6_MAX_SURF, (const uint32_t (*)[3])big); CHECK("parse: 16 entries fit exactly", n48m6_parse(&s, c, m16) == 0 && s.n == N48M6_MAX_SURF); n48m6_parse(&s, b, n); }
      CHECK("parse: every refusal KEEPS the old table", memcmp(&keep.e, &s.e, sizeof s.e) == 0 && keep.n == s.n);
      CHECK("parse: NULL arguments", n48m6_parse(NULL, b, n) == -1 && n48m6_parse(&s, NULL, n) == -1); }
    // ---- the decisions with the latch OFF: build 10, whatever the table says
    { n48m6_t o; memset(&o, 0, sizeof o); const uint32_t e[2][3] = { { 201, 2, 0 }, { 301, 0, N48M6_ENT_AMBIGUOUS } }; size_t n = blob(b, 2, e); n48m6_parse(&o, b, n);
      CHECK("latch OFF: the gate presents a surface the table maps to the monitor B", n48m6_gate(&o, 201) == N48M6_PRESENT);
      CHECK("latch OFF: the gate presents an ambiguous surface", n48m6_gate(&o, 301) == N48M6_PRESENT);
      CHECK("latch OFF: the gate presents an unknown surface", n48m6_gate(&o, 999) == N48M6_PRESENT);
      CHECK("latch OFF: the completion presents all of them", n48m6_final(&o, 201) == N48M6_PRESENT && n48m6_final(&o, 301) == N48M6_PRESENT && n48m6_final(&o, 999) == N48M6_PRESENT);
      CHECK("latch OFF: nothing is counted", o.gate_ok == 0 && o.gate_tentative == 0 && o.fin_ok == 0 && o.fin_unknown == 0); }
    // ---- the decisions with the latch ON
    { n48m6_t o; memset(&o, 0, sizeof o); o.latch = 1;
      CHECK("latch ON, no table yet: the gate lets a surface through TENTATIVELY", n48m6_gate(&o, 101) == N48M6_TENTATIVE && o.gate_tentative == 1);
      CHECK("latch ON, no table yet: the completion presents NOTHING", n48m6_final(&o, 101) == N48M6_SKIP_UNKNOWN && o.fin_unknown == 1);
      const uint32_t e[6][3] = { { 101, 0, 0 }, { 102, 0, 0 }, { 103, 0, 0 }, { 201, 2, 0 }, { 202, 2, 0 }, { 301, 0, N48M6_ENT_AMBIGUOUS } }; size_t n = blob(b, 6, e); n48m6_parse(&o, b, n);
      memset(&o.gate_skip, 0, sizeof o.gate_skip); o.gate_ok = o.gate_tentative = 0;
      CHECK("latch ON: the DP's surface passes the gate", n48m6_gate(&o, 101) == N48M6_PRESENT && o.gate_ok == 1);
      CHECK("latch ON: a monitor B surface takes NO slot (instance 2)", n48m6_gate(&o, 201) == N48M6_SKIP_OTHER && o.gate_skip[2] == 1 && o.gate_skip[1] == 0);
      { const uint32_t a[1][3] = { { 401, 1, 0 } }; uint8_t c[64]; size_t m = blob(c, 1, a); n48m6_t p = o; n48m6_parse(&p, c, m); CHECK("latch ON: an monitor A surface takes NO slot (instance 1)", n48m6_gate(&p, 401) == N48M6_SKIP_OTHER && p.gate_skip[1] == 1); }
      CHECK("latch ON: an AMBIGUOUS surface takes no slot", n48m6_gate(&o, 301) == N48M6_SKIP_AMBIG && o.gate_ambig == 1);
      CHECK("latch ON: an unknown surface is let through tentatively", n48m6_gate(&o, 777) == N48M6_TENTATIVE && o.gate_tentative == 1);
      CHECK("latch ON: surface ID 0 is unknown", n48m6_gate(&o, 0) == N48M6_TENTATIVE && n48m6_final_peek(&o, 0) == N48M6_SKIP_UNKNOWN);
      CHECK("latch ON: the completion presents the DP's surface", n48m6_final(&o, 102) == N48M6_PRESENT && o.fin_ok == 1);
      CHECK("latch ON: the completion refuses a monitor B surface", n48m6_final(&o, 202) == N48M6_SKIP_OTHER && o.fin_skip[2] == 1);
      CHECK("latch ON: the completion refuses an ambiguous surface", n48m6_final(&o, 301) == N48M6_SKIP_AMBIG && o.fin_ambig == 1);
      CHECK("latch ON: the completion refuses an unknown surface (after the caller's refresh)", n48m6_final(&o, 888) == N48M6_SKIP_UNKNOWN && o.fin_unknown == 2);
      CHECK("peek counts nothing", ({ uint64_t before = o.fin_ok + o.fin_unknown + o.fin_ambig; (void)n48m6_final_peek(&o, 101); (void)n48m6_final_peek(&o, 888); before == o.fin_ok + o.fin_unknown + o.fin_ambig; }));
      CHECK("only PRESENT may present", n48m6_may_present(N48M6_PRESENT) && !n48m6_may_present(N48M6_TENTATIVE) && !n48m6_may_present(N48M6_SKIP_OTHER) && !n48m6_may_present(N48M6_SKIP_AMBIG) && !n48m6_may_present(N48M6_SKIP_UNKNOWN));
      // THE misroute this build exists to stop: a 2560x1440 monitor B surface is size-matched to the DP's plane; with the kernel's table it is never presented
      CHECK("a monitor B surface that matches HUBP0's size is never presented, at either stage", n48m6_gate(&o, 201) != N48M6_PRESENT && n48m6_final(&o, 201) != N48M6_PRESENT && n48m6_gate(&o, 202) != N48M6_PRESENT && n48m6_final(&o, 202) != N48M6_PRESENT);
      CHECK("a refresh that now knows the surface turns the tentative frame into a present", ({ const uint32_t a2[1][3] = { { 888, 0, 0 } }; uint8_t c[64]; size_t m = blob(c, 1, a2); n48m6_parse(&o, c, m); n48m6_final(&o, 888) == N48M6_PRESENT; }));
      CHECK("... and a refresh that maps it elsewhere turns it into a skip", ({ const uint32_t a3[1][3] = { { 888, 2, 0 } }; uint8_t c[64]; size_t m = blob(c, 1, a3); n48m6_parse(&o, c, m); n48m6_final(&o, 888) == N48M6_SKIP_OTHER; })); }
    // ---- bundle 12 (queue 290 S3): the completion plan, decided from the table as it is, with no sleep and no read
    { n48m6_t o; memset(&o, 0, sizeof o);
      CHECK("plan: latch OFF presents everything, whatever the table says", n48m6_complete_plan(&o, 5, 0) == N48M6_C_PRESENT && n48m6_complete_plan(&o, 0, 99) == N48M6_C_PRESENT);
      o.latch = 1;
      const uint32_t e[4][3] = { { 101, 0, 0 }, { 201, 2, 0 }, { 401, 1, 0 }, { 301, 0, N48M6_ENT_AMBIGUOUS } }; size_t n = blob(b, 4, e); n48m6_parse(&o, b, n);
      CHECK("plan: instance 0 -> PRESENT", n48m6_complete_plan(&o, 101, 0) == N48M6_C_PRESENT && n48m6_complete_plan(&o, 101, 7) == N48M6_C_PRESENT);
      CHECK("plan: the monitor B's / the monitor A's / an ambiguous surface -> SKIP (at every try)", n48m6_complete_plan(&o, 201, 0) == N48M6_C_SKIP && n48m6_complete_plan(&o, 401, 3) == N48M6_C_SKIP && n48m6_complete_plan(&o, 301, 0) == N48M6_C_SKIP && n48m6_complete_plan(&o, 201, 99) == N48M6_C_SKIP);
      { int ok = 1; for (int t = 0; t < N48M6_RETRIES; t++) ok = ok && n48m6_complete_plan(&o, 999, t) == N48M6_C_RETRY; CHECK("plan: an UNKNOWN surface is RETRIED for tries 0..7", ok); }
      CHECK("plan: ... and DROPPED at the last try (8), never presented", n48m6_complete_plan(&o, 999, N48M6_RETRIES) == N48M6_C_DROP && n48m6_complete_plan(&o, 999, 50) == N48M6_C_DROP && n48m6_complete_plan(&o, 0, 0) == N48M6_C_RETRY);
      CHECK("plan: it counts NOTHING (the caller counts once, at the end)", o.fin_ok == 0 && o.fin_unknown == 0 && o.fin_ambig == 0 && o.gate_ok == 0);
      CHECK("plan: a refresh that now knows the ID turns a RETRY into a PRESENT", ({ const uint32_t a2[1][3] = { { 999, 0, 0 } }; uint8_t c[64]; size_t m = blob(c, 1, a2); n48m6_parse(&o, c, m); n48m6_complete_plan(&o, 999, 3) == N48M6_C_PRESENT; }));
      CHECK("plan: the empty table (just reset) retries too", ({ n48m6_t z; memset(&z, 0, sizeof z); z.latch = 1; n48m6_complete_plan(&z, 5, 0) == N48M6_C_RETRY; })); }
    // ---- constants: the bundle's copy equals the kernel's
    CHECK("retry plan", N48M6_RETRIES == 8 && N48M6_RETRY_NS == 3000000ull && N48M6_C_PRESENT == 0 && N48M6_C_SKIP == 1 && N48M6_C_RETRY == 2 && N48M6_C_DROP == 3 && N48M6_MAX_SURF == 16u && N48M6_NINST == 3u);
    CHECK("the scan_query flag is the kernel's bit 5", N48M6_SCANQ_FLAG == N48N_SCANQ_M6 && N48N_SCANQ_M6 == (1u << 5) && N48N_SCANQ_M6 != N48N_SCANQ_DTO_VALID);
    { char *kp = slurp(dir, "../../../src/navi48-bringup/src/amd/native_m6_pure.h"), *ka = slurp(dir, "../../../src/navi48-bringup/src/Navi48NativeABI.h");
      CHECK("kernel: blob version / header / entry size / table size", strstr(kp, "kBlobVersion = 1u, kBlobHdr = 8u, kBlobEnt = 12u") && strstr(kp, "constexpr uint32_t kMaxSurf = 16u;") && strstr(kp, "constexpr uint32_t kEntAmbiguous = 1u;"));
      CHECK("kernel: the property names", strstr(kp, "#define N48M6_PROP_SURF    \"" N48M6_PROP_SURF "\"") && strstr(kp, "#define N48M6_PROP_LATCH   \"" N48M6_PROP_LATCH "\""));
      CHECK("kernel: instance numbers (DP 0, monitor A 1, monitor B 2)", strstr(kp, "kInstDp = 0u, kInstMonA = 1u, kInstMonB = 2u") && N48M6_INST_DP == 0 && N48M6_INST_MONA == 1 && N48M6_INST_MONB == 2);
      CHECK("kernel ABI header carries N48N_SCANQ_M6 at bit 5", strstr(ka, "#define N48N_SCANQ_M6         (1u << 5)") != NULL);
      free(kp); free(ka); }
    // ---- the glue in Navi48Device.m, in the order that makes the decisions reachable
    { char *m = slurp(dir, "Navi48Device.m"), *pl = slurp(dir, "Info.plist"), *ins = slurp(dir, "INSTALL.md");
      const char *ga = strstr(m, "static BOOL n48s_disp_account("); const char *fa = ga ? strstr(ga, "int nf0 = am->nflag_log, ok = n48df_account_sid(") : NULL; const char *gg = ga ? strstr(ga, "n48x_plan_inst(&N48S.m6, sid, xon, &tent)") : NULL;
      CHECK("glue (bundle 20, F1): the account call passes known = latch on and NOT tentative", ga && strstr(ga, "n48df_account_sid(am, mask, sid, N48S.m6.latch && !tent)") != NULL);
      CHECK("glue: the gate runs inside n48s_disp_account, under the lock, BEFORE the frame is accounted (before it can take a slot)", ga && fa && gg && gg < fa && strstr(ga, "if (pi == N48X_PLAN_SKIP) { pthread_mutex_unlock(&N48S.mu); return NO; }") && strstr(ga, "if (N48S.m6.latch) {") < gg);
      const char *ca = strstr(m, "static void n48s_complete_run(int inst, int slot, VkResult r, uint64_t seq, uint32_t sid, n48crc_rec_t *rec, int cls, uint64_t tcommit, int tries) {");
      const char *cp = ca ? strstr(ca, "n48x_complete_plan(&N48S.m6, sid, inst, tries, stale, xon)") : NULL, *hh = ca ? strstr(ca, "int hok = n48df_complete_held(") : NULL, *fc = ca ? strstr(ca, "n48m6_final_count(&N48S.m6, sid, N48M6_PRESENT)") : NULL;
      const char *crc = ca ? strstr(ca, "if (rec) n48s_crc_present(rec);") : NULL, *pres = ca ? strstr(ca, "int rc = xinst ? n48x_k_present(inst, sids[slot], o) : N48S.fp(N48R.dev, sids[slot], o);") : NULL, *ra = ca ? strstr(ca, "dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)N48M6_RETRY_NS), N48S.pq,") : NULL, *sk = ca ? strstr(ca, "n48s_m6_skip_locked(slot, sid, plan, sm);") : NULL;
      CHECK("glue (S3): the M6 verdict is decided BEFORE n48df_complete_held (which releases the slot), and the present comes after the state machine, the count and the CRC hook", ca && cp && hh && fc && crc && pres && cp < hh && hh < fc && fc < crc && crc < pres);
      CHECK("glue (S3): an unknown ID is queued again with dispatch_after on the present queue and the table refreshed ASYNCHRONOUSLY, a skip releases through n48df_skip_content, both BEFORE complete_held", ra && sk && cp < ra && ra < hh && sk < hh && strstr(ca, "n48s_m6_refresh_async();") && strstr(ca, "n48s_m6_refresh_async();") < ra && strstr(ca, "tries + 1"));
      { const char *e1 = ca ? strstr(ca, "// Device selectors for mtlprobe dispflip") : NULL; char *seg = ca && e1 ? strndup(ca, (size_t)(e1 - ca)) : strdup("");
        CHECK("glue (S3): the completion path never sleeps and never reads the registry or drops the lock for it", strstr(seg, "nanosleep") == NULL && strstr(seg, "n48s_m6_read(") == NULL && strstr(seg, "n48s_m6_refresh_locked(") == NULL && strstr(seg, "usleep") == NULL && countof(seg, "pthread_mutex_unlock(&N48S.mu)") >= 3 && countof(seg, "pthread_mutex_lock(&N48S.mu)") == 1 && countof(seg, "n48df_complete_held(") == 1 && countof(seg, "n48df_skip_content(") == 0 && countof(seg, "n48s_crc_free(rec);") == 2); free(seg); }
      CHECK("glue (S3): bundle 11's sleeping final decision is gone", strstr(m, "n48s_m6_final_locked") == NULL);
      { const char *ra2 = strstr(m, "static void n48s_m6_refresh_async(void) {"); const char *rd = ra2 ? strstr(ra2, "n48s_m6_read(&tmp)") : NULL, *lk = ra2 ? strstr(ra2, "pthread_mutex_lock(&N48S.mu);") : NULL;
        CHECK("glue (S3): the async refresh reads the registry with NO lock held, stores under N48S.mu, and runs at most one at a time", ra2 && rd && lk && rd < lk && strstr(ra2, "if (rc == 0) { if (n48m6_store(&N48S.m6, &tmp) != 0 && N48S.m6.store_refused <= 3)") && strstr(ra2, "atomic_exchange(&n48s_m6_refreshing, 1)") && strstr(ra2, "dispatch_get_global_queue(QOS_CLASS_UTILITY, 0)")); }
      { const char *sl = strstr(m, "static void n48s_m6_skip_locked(int slot, uint32_t sid, int plan, n48df_t *sm) {");
        CHECK("glue (S3): a skipped frame is counted once, logged (first 12) and its slot released through n48df_skip_content", sl && strstr(sl, "n48df_skip_content(sm, slot);") && strstr(sl, "n48m6_final_count(&N48S.m6, sid,") && strstr(sl, "if (lg < 12)")); }
      const char *rf2 = strstr(m, "static void n48s_m6_refresh_locked(void) {"); const char *u1 = rf2 ? strstr(rf2, "pthread_mutex_unlock(&N48S.mu);") : NULL, *rd = rf2 ? strstr(rf2, "n48s_m6_read(&tmp)") : NULL, *l1 = rf2 ? strstr(rf2, "pthread_mutex_lock(&N48S.mu);") : NULL;
      CHECK("glue: the report's registry read happens with N48S.mu DROPPED", rf2 && u1 && rd && l1 && u1 < rd && rd < l1);
      CHECK("glue: a successful refresh stores through n48m6_store (entries, count, table present, and since build 13 the generation, never an older one over a newer one), in BOTH refresh paths", countof(m, "if (rc == 0) { if (n48m6_store(&N48S.m6, &tmp) != 0 && N48S.m6.store_refused <= 3)") == 2 && countof(m, "memcpy(N48S.m6.e") == 0);
      CHECK("glue: the table is the Metal nub's property, read through the accelerator port's parent, parsed by n48m6_parse", strstr(m, "CFSTR(N48M6_PROP_SURF)") && strstr(m, "IORegistryEntryGetParentEntry((io_registry_entry_t)port, kIOServicePlane, &parent)") && strstr(m, "rc = n48m6_parse(tmp, CFDataGetBytePtr((CFDataRef)v), (size_t)CFDataGetLength((CFDataRef)v));"));
      CHECK("glue: the latch comes from scan_query's flags and from nowhere else", strstr(m, "N48S.m6.latch = (q.flags & N48N_SCANQ_M6) != 0;") && countof(m, "N48S.m6.latch =") == 1);
      CHECK("glue: only WindowServer's admitted device records the accelerator port", strstr(m, "if (self && n48_is_ws() && !n48_acc_port) n48_acc_port = port;") != NULL);
      CHECK("glue: the periodic counter log carries 'surfaces for instance 1/2' and runs from the T1 report", strstr(m, "m6: surfaces for instance 1/2: %u/%u") && strstr(m, "n48_m6_report();   // bundle 11"));
      CHECK("glue: n48s_classify is unchanged (the size match still classifies; the table decides which display)", strstr(m, "ok = size && pitch && (bt || test);") != NULL);
      CHECK("bundle build 20 in Info.plist", strstr(pl, "<key>CFBundleVersion</key>\n\t<string>20</string>") != NULL);
      CHECK("INSTALL.md documents builds 11, 12 and 13 and the channel", strstr(ins, "Build 11 (CFBundleVersion 11") && strstr(ins, "Build 12 (CFBundleVersion 12") && strstr(ins, "Build 13 (CFBundleVersion 13") && strstr(ins, "n48df_skip_content") && strstr(ins, "Navi48,M6Surf") && strstr(ins, "N48N_SCANQ_M6") && strstr(ins, "navi48-m6"));
      free(m); free(pl); free(ins); }
    printf("test-m6route: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
