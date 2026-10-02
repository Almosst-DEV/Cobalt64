// gfx_wc98_test.cpp — build 0.0.541 (; switch 98, gfx_wc98.h): the provenance-ask walk cache.
//     clang++ -std=c++17 -Wall -Wextra -Werror -O1 -fsanitize=address,undefined -fno-sanitize-recover=all \
//         -I src/navi48-bringup/src/apple src/navi48-bringup/tests/gfx_wc98_test.cpp -o /tmp/wc98 && /tmp/wc98 \
//         src/navi48-bringup/src/apple/AppleHardwareHook.cpp src/navi48-bringup/src/Navi48Bringup.cpp
// Covers:
//   W1 SHADOW disagreement 0 (and ON answer == the uncached walk, always) across randomized remap schedules against an ORACLE page
//      table that mirrors gfxc_page_walk (root / 64 KiB L1 leaf / 4 KiB sub-table leaf), with EVERY bump site exercised, lookups
//      made while remaps are in flight, and a remap landing in the middle of a walk;
//   W2 a STALE-ENTRY PLANT (a remap path that forgets to bump) IS caught: SHADOW counts the disagreement and uses the walk's answer;
//      ON would have answered stale (the reason ON must stay behind SHADOW evidence);
//   W3 OFF identity: OFF never opens a scope; a runtime model of gfxc_page's dispatch walks every call and moves no counter;
//   W4 a failed walk is never cached; an unstable bracket (generation moved / remap in flight) stores nothing and never hits;
//   W5 scope ("pass") scoping: an entry never answers in a later scope, whatever the generation says;
//   W6 the 64 KiB granule: 1 walk + 15 hits per 64 KiB leaf; a 4 KiB sub-table leaf and an unaligned START key by the page;
//   W7 the kext glue (source pins): OFF at boot, the verb the only writer, the guard; gfxc_page's dispatch; the scope opened ONLY in
//      gfxsrc_desc_tiled_ok (so the gate's keystone, cprov and R1 walks are never cached); every bump site; gfxc_page_walk unchanged
//      since 0.0.540; every MM-window write inside a copy scope;
//   W8 every walkcache98 line <= 491 bytes at maximal fields.
//   W9 build 0.0.543 item B (switch 101, the instability census): NO BEHAVIOUR CHANGE (40 randomized schedules, census ON vs
//      OFF: identical answers, counters and cache contents); attribution of each unstable walk to its bracket class (mapVA / unmapVA /
//      copy guard in flight or moved mid-walk; a keystone bump and a ctx release as OTHER; NONE); census OFF writes nothing; the kext
//      glue (the verb the only writer, the per-walk perf540 count, the STOP line after 98's, the per-second line after perf540's);
//      both census lines <= 491 bytes.
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <map>
#include <vector>
#include "gfx_wc98.h"
#include "gfx_commit.h"
#include "gfx_perf540.h"   /* build 0.0.543 item B: the per-second census line's format */

#ifndef N48_LOG_CAP_BODY
#define N48_LOG_CAP_BODY 491u
#endif

static int gFail = 0, gRun = 0;
static void expect_u(const char *what, uint64_t got, uint64_t want)
{
    gRun++;
    if (got != want) { gFail++; std::printf("FAIL  %-110s got %#llx want %#llx\n", what, (unsigned long long)got, (unsigned long long)want); }
    else std::printf("ok    %-110s %#llx\n", what, (unsigned long long)got);
}
static std::string slurp(const char *p)
{
    std::string s; FILE *f = p ? std::fopen(p, "rb") : nullptr;
    if (!f) return s;
    char b[65536]; size_t n;
    while ((n = std::fread(b, 1, sizeof b, f)) > 0) s.append(b, n);
    std::fclose(f);
    return s;
}
static uint32_t count(const std::string &s, const std::string &n)
{
    uint32_t c = 0; size_t p = 0;
    if (n.empty()) return 0;
    while ((p = s.find(n, p)) != std::string::npos) { c++; p += n.size(); }
    return c;
}
static std::string body_of(const std::string &s, const std::string &head)
{
    const size_t a = s.find(head);
    if (a == std::string::npos) return std::string();
    const size_t e = s.find("\n}\n", a);
    return s.substr(a, e == std::string::npos ? std::string::npos : e - a);
}
static uint64_t gRs = 0x2545F4914F6CDD1Dull;
static uint32_t rnd() { gRs ^= gRs << 13; gRs ^= gRs >> 7; gRs ^= gRs << 17; return (uint32_t)(gRs >> 11); }

/* ---- THE ORACLE: VRAM as 8-byte entries, and gfxc_page_walk's walk over it, line for line (the kext's reads become map reads;
 * vmib_to_vram_off is the kext's: `off = addr - fbStart` when addr >= fbStart, ok when off + 0x1000 <= vramSize). ---- */
struct Vram { std::map<uint64_t, uint64_t> m; uint64_t reads = 0; };
static Vram gV;
struct Vm { uint64_t rootOff, startVa, fbStart, vramSize, root; bool ok; };
static uint64_t rd(uint64_t off) { gV.reads++; auto it = gV.m.find(off); return it == gV.m.end() ? 0ull : it->second; }
static uint64_t vmib(uint64_t addr, uint64_t fb, uint64_t sz, bool &ok) { uint64_t off = addr; if (fb && addr >= fb) off = addr - fb; ok = (off + 0x1000ull) <= sz; return off; }
static bool oracle_walk(const Vm &vm, uint64_t va, uint64_t &pageBase, bool &isSys, uint64_t *leafOut, uint32_t *lv)
{
    if (leafOut) *leafOut = 0ull;
    if (!vm.ok || va < vm.startVa) return false;
    bool ok = false;
    const uint32_t rootIdx = (uint32_t)((va - vm.startVa) >> 28);
    if (lv && rootIdx < 512) (*lv)++;
    if (rootIdx >= 512) return false;
    const uint64_t rootEnt = rd(vm.rootOff + (uint64_t)rootIdx * 8u);
    if (!(rootEnt & 1ull)) return false;
    const uint64_t l1Base = vmib(rootEnt & 0x0000FFFFFFFFF000ull, vm.fbStart, vm.vramSize, ok);
    if (lv && ok) (*lv)++;
    if (!ok) return false;
    const uint64_t l1Ent = rd(l1Base + ((va >> 16) & 0xfffull) * 8u);
    if (!(l1Ent & 1ull)) return false;
    uint64_t leaf = l1Ent;
    bool viaSub = false;
    if (!((l1Ent >> 63) & 1ull)) {
        const uint64_t subBase = vmib(l1Ent & 0x0000FFFFFFFFFF80ull, vm.fbStart, vm.vramSize, ok);
        if (lv && ok) (*lv)++;
        if (!ok) return false;
        leaf = rd(subBase + ((va >> 12) & 0xfull) * 8u);
        viaSub = true;
    }
    if (!(leaf & 1ull)) return false;
    const uint64_t leafAddr = leaf & 0x0000FFFFFFFFF000ull;
    isSys = ((leaf >> 1) & 1ull) != 0;
    pageBase = viaSub ? leafAddr : ((leafAddr + (va & 0xf000ull)) & ~0xfffull);
    if (leafOut) *leafOut = leaf;
    return true;
}

/* The page table's builder. Root at 0x100000; L1 tables at 0x200000 + rootIdx * 0x8000; sub-tables allocated upward. */
static const uint64_t kRoot = 0x100000ull;
static uint64_t gSubNext = 0x4000000ull;
static void pt_reset() { gV.m.clear(); gSubNext = 0x4000000ull; }
static uint64_t l1_of(const Vm &vm, uint64_t va) {
    const uint32_t ri = (uint32_t)((va - vm.startVa) >> 28);
    const uint64_t l1 = 0x200000ull + (uint64_t)ri * 0x8000ull;
    gV.m[vm.rootOff + ri * 8ull] = l1 | 1ull;
    return l1;
}
static void map_big(const Vm &vm, uint64_t va, uint64_t phys, uint32_t sys) {
    const uint64_t l1 = l1_of(vm, va);
    gV.m[l1 + ((va >> 16) & 0xfffull) * 8u] = (phys & 0x0000FFFFFFFF0000ull) | 1ull | ((uint64_t)sys << 1) | (1ull << 63);
}
static void map_small(const Vm &vm, uint64_t va, uint64_t phys, uint32_t sys) {
    const uint64_t l1 = l1_of(vm, va);
    const uint64_t slot = l1 + ((va >> 16) & 0xfffull) * 8u;
    uint64_t e = gV.m.count(slot) ? gV.m[slot] : 0ull;
    if (!(e & 1ull) || ((e >> 63) & 1ull)) { e = gSubNext | 1ull; gSubNext += 0x1000ull; gV.m[slot] = e; }
    const uint64_t sub = e & 0x0000FFFFFFFFFF80ull;
    gV.m[sub + ((va >> 12) & 0xfull) * 8u] = (phys & 0x0000FFFFFFFFF000ull) | 1ull | ((uint64_t)sys << 1);
}
static void unmap_gran(const Vm &vm, uint64_t va) {
    const uint64_t l1 = l1_of(vm, va);
    gV.m[l1 + ((va >> 16) & 0xfffull) * 8u] = 0ull;
}

/* The cache's walk: the oracle's, counted, with an optional mid-walk remap (a remap landing while a walk runs). */
struct WalkCtx { const Vm *vm; uint64_t walks; void (*mid)(void); };
static uint32_t cb_walk(void *ctx, uint64_t va, uint64_t *page, uint32_t *sys, uint64_t *leaf, uint32_t *levels)
{
    WalkCtx *c = static_cast<WalkCtx *>(ctx);
    c->walks++;
    uint64_t pb = 0ull, lf = 0ull; bool sy = false; uint32_t lv = 0u;
    const bool ok = oracle_walk(*c->vm, va, pb, sy, &lf, &lv);
    if (c->mid) { void (*m)(void) = c->mid; c->mid = nullptr; m(); }
    *page = pb; *sys = sy ? 1u : 0u; *leaf = lf; *levels = lv;
    return ok ? 1u : 0u;
}
static n48_wc_vmk vmk_of(const Vm &vm) { return n48_wc_vmk { vm.rootOff, vm.startVa, vm.fbStart, vm.vramSize, vm.ok ? 1ull : 0ull }; }

static n48_wc gC;
static n48_wc_gen gG;
static const Vm kVmA { kRoot, 0x400000000ull, 0ull, 1ull << 36, kRoot, true };   /* START 64 KiB aligned */
static const Vm kVmU { kRoot, 0x400001000ull, 0ull, 1ull << 36, kRoot, true };   /* START 4 KiB only: small keys */

/* One lookup through the cache; *mis 1 when the answer used differs from the oracle's answer RIGHT NOW. */
static uint32_t look(const Vm &vm, uint64_t va, WalkCtx &wc, uint32_t *mis, uint32_t *hit)
{
    wc.vm = &vm;   /* the walk is always over the vm being looked up */
    const n48_wc_vmk k = vmk_of(vm);
    uint64_t pg = 0, lf = 0; uint32_t sy = 0, h = 0, di = N48_WC_DIS;
    const uint32_t ok = n48_wc_page(&gC, &gG, &k, va, &cb_walk, &wc, &pg, &sy, &lf, &h, &di);
    uint64_t op = 0, ol = 0; bool os = false;
    const uint64_t r0 = gV.reads;
    const bool ook = oracle_walk(vm, va, op, os, &ol, nullptr);
    gV.reads = r0;
    const uint32_t same = (ok == (ook ? 1u : 0u)) && (!ok || (pg == op && sy == (os ? 1u : 0u) && lf == ol));
    if (mis) *mis = same ? 0u : 1u;
    if (hit) *hit = h;
    return ok;
}

/* ---- W1: randomized remap schedules, every bump site ---- */
enum { R_UNMAP = 0, R_MAP, R_CG, R_KSW, R_KSA, R_REBIND, R_CTXREL, R_WSV, R_SDMA, R_N };
static const uint32_t kSiteOf[R_N][2] = {
    { N48_WC_B_UNMAP_IN, N48_WC_B_UNMAP_OUT }, { N48_WC_B_MAP_IN, N48_WC_B_MAP_OUT }, { N48_WC_B_CG_OPEN, N48_WC_B_CG_END },
    { N48_WC_B_KS_WITHDRAW, N48_WC_B_KS_WITHDRAW }, { N48_WC_B_KS_ARM, N48_WC_B_KS_ARM }, { N48_WC_B_REBIND, N48_WC_B_REBIND },
    { N48_WC_B_CTX_RELEASE, N48_WC_B_CTX_RELEASE }, { N48_WC_B_WSV_BASE, N48_WC_B_WSV_BASE },
    { N48_WC_B_SDMA_SUBMIT, N48_WC_B_SDMA_SUBMIT } };
static std::vector<uint64_t> gPool;
static void remap_one(const Vm &vm)
{
    const uint64_t va = gPool[rnd() % gPool.size()];
    const uint64_t phys = 0x10000000ull + (uint64_t)(rnd() % 4096u) * 0x10000ull;
    switch (rnd() % 3u) {
    case 0: map_big(vm, va & ~0xffffull, phys, rnd() % 2u); break;
    case 1: map_small(vm, va & ~0xfffull, phys + (rnd() % 16u) * 0x1000ull, rnd() % 2u); break;
    default: unmap_gran(vm, va); break;
    }
}
static uint32_t gSites[R_N];
static uint32_t gCensusRun = 0u;
static void w1_run(uint32_t mode, const Vm &vm, uint32_t steps, uint64_t *mismatch, uint64_t *disagree, uint64_t *hits)
{
    pt_reset();
    gC = n48_wc {}; gG = n48_wc_gen {};
    gC.census = gCensusRun;   /* build 0.0.543 item B: W9 runs the same schedules with the census ON */
    gPool.clear();
    for (uint32_t i = 0; i < 48u; i++) {
        const uint64_t base = 0x400000000ull + (uint64_t)(rnd() % 64u) * 0x10000ull;
        gPool.push_back(base + (uint64_t)(rnd() % 16u) * 0x1000ull);
        if (i % 3u == 0u) map_big(vm, base, 0x10000000ull + (uint64_t)i * 0x10000ull, i & 1u);
        else if (i % 3u == 1u) map_small(vm, base + (uint64_t)(rnd() % 16u) * 0x1000ull, 0x20000000ull + i * 0x1000ull, 0u);
    }
    WalkCtx wc { &vm, 0, nullptr };
    uint32_t open = 0;
    uint64_t mis = 0;
    uint64_t cur = gPool[0] & ~0xffffull;
    uint32_t sweep = 0u;
    for (uint32_t s = 0; s < steps; s++) {
        const uint32_t op = rnd() % 32u;
        if (op == 0u) { if (open) { n48_wc_scope_close(&gC, 0u); open = 0; } else open = n48_wc_scope_open(&gC, mode, 7u); continue; }
        if (!open) { open = n48_wc_scope_open(&gC, mode, 7u); }
        if (op <= 27u) {   /* a lookup: the proof loop's shape - consecutive pages of one granule, then another pooled granule */
            if ((sweep & 15u) == 0u) cur = (gPool[rnd() % gPool.size()] & ~0xffffull);
            const uint64_t va = cur + (uint64_t)(sweep & 15u) * 0x1000ull;
            sweep++;
            uint32_t m = 0; (void)look(vm, va, wc, &m, nullptr); mis += m;
            continue;
        }
        const uint32_t r = rnd() % R_N;
        gSites[r]++;
        if (r <= R_CG) {   /* a bracketed remap: in flight across lookups, the change in the middle */
            n48_wc_enter(&gG, kSiteOf[r][0]);
            for (uint32_t k = rnd() % 3u; k; k--) { uint32_t m = 0; (void)look(vm, gPool[rnd() % gPool.size()], wc, &m, nullptr); mis += m; }
            remap_one(vm);
            for (uint32_t k = rnd() % 3u; k; k--) { uint32_t m = 0; (void)look(vm, gPool[rnd() % gPool.size()], wc, &m, nullptr); mis += m; }
            n48_wc_leave(&gG, kSiteOf[r][1]);
        } else {           /* a single bump: the change, then the bump, with no lookup between (the kext's order) */
            remap_one(vm);
            n48_wc_bump(&gG, kSiteOf[r][0]);
        }
    }
    if (open) n48_wc_scope_close(&gC, 0u);
    *mismatch = mis; *disagree = gC.st.disagree; *hits = gC.st.hits;
}
static void w1()
{
    uint64_t mis = 0, dis = 0, hits = 0, tot = 0, disTot = 0, hitTot = 0;
    for (uint32_t run = 0; run < 40u; run++) {
        w1_run(N48_WC_SHADOW, run & 1u ? kVmU : kVmA, 4000u, &mis, &dis, &hits);
        tot += mis; disTot += dis; hitTot += hits;
    }
    expect_u("W1 SHADOW, 40 randomized schedules x 4000 steps: every answer USED is the uncached walk's (mismatches)", tot, 0u);
    expect_u("W1 SHADOW: disagreements between the cache and the walk (every remap bumps)", disTot, 0u);
    expect_u("W1 SHADOW: the schedules did compare hits (non-vacuous)", hitTot > 1000u ? 1u : 0u, 1u);
    tot = 0; hitTot = 0;
    for (uint32_t run = 0; run < 40u; run++) {
        w1_run(N48_WC_ON, run & 1u ? kVmU : kVmA, 4000u, &mis, &dis, &hits);
        tot += mis; hitTot += hits;
    }
    expect_u("W1 ON, 40 randomized schedules: every answer (hits included) equals the oracle walk at that instant", tot, 0u);
    expect_u("W1 ON: hits answered from the cache (non-vacuous)", hitTot > 1000u ? 1u : 0u, 1u);
    uint32_t every = 1u;
    for (uint32_t r = 0; r < R_N; r++) if (gSites[r] < 50u) every = 0u;
    expect_u("W1 every bump site exercised >= 50 times (unmapVA, mapVA, copy open/END, keystone withdraw/arm, rebind, ctx release, "
             "ws-valid base, SDMA submit)", every, 1u);
    expect_u("W1 every site's counter moved in the generation", (gG.site[N48_WC_B_UNMAP_IN] && gG.site[N48_WC_B_UNMAP_OUT] &&
             gG.site[N48_WC_B_MAP_IN] && gG.site[N48_WC_B_MAP_OUT] && gG.site[N48_WC_B_CG_OPEN] && gG.site[N48_WC_B_CG_END] &&
             gG.site[N48_WC_B_KS_WITHDRAW] && gG.site[N48_WC_B_KS_ARM] && gG.site[N48_WC_B_REBIND] && gG.site[N48_WC_B_CTX_RELEASE] &&
             gG.site[N48_WC_B_WSV_BASE] && gG.site[N48_WC_B_SDMA_SUBMIT]) ? 1u : 0u, 1u);

    /* a remap LANDING IN THE MIDDLE OF A WALK (bracketed, completed before the walk returns): nothing is stored */
    pt_reset(); gC = n48_wc {}; gG = n48_wc_gen {};
    map_big(kVmA, 0x400050000ull, 0x30000000ull, 0u);
    WalkCtx wc { &kVmA, 0, nullptr };
    (void)n48_wc_scope_open(&gC, N48_WC_ON, 7u);
    wc.mid = []() { n48_wc_enter(&gG, N48_WC_B_MAP_IN); map_big(kVmA, 0x400050000ull, 0x31000000ull, 0u); n48_wc_leave(&gG, N48_WC_B_MAP_OUT); };
    uint32_t m1 = 0, h1 = 0, m2 = 0, h2 = 0;
    (void)look(kVmA, 0x400050000ull, wc, &m1, &h1);
    (void)look(kVmA, 0x400051000ull, wc, &m2, &h2);
    expect_u("W1 a remap completing inside a walk: that walk is not stored (unstable 1) and the next page misses", gC.st.unstable == 1u &&
             h2 == 0u ? 1u : 0u, 1u);
    expect_u("W1 ... and the next page's answer is the NEW mapping's", m2, 0u);
    n48_wc_scope_close(&gC, 0u);

    /* a lookup while a remap is IN FLIGHT: no hit, no store */
    pt_reset(); gC = n48_wc {}; gG = n48_wc_gen {};
    map_big(kVmA, 0x400060000ull, 0x32000000ull, 0u);
    (void)n48_wc_scope_open(&gC, N48_WC_ON, 7u);
    uint32_t mm = 0, hh = 0;
    (void)look(kVmA, 0x400060000ull, wc, &mm, &hh);                 /* stored */
    n48_wc_enter(&gG, N48_WC_B_UNMAP_IN);
    (void)look(kVmA, 0x400061000ull, wc, &mm, &hh);
    expect_u("W1 in flight: no hit (the stored granule is not answered while a remap is in flight)", hh, 0u);
    const uint64_t st0 = gC.st.stored;
    (void)look(kVmA, 0x400062000ull, wc, &mm, &hh);
    expect_u("W1 in flight: nothing stored", gC.st.stored, st0);
    n48_wc_leave(&gG, N48_WC_B_UNMAP_OUT);
    n48_wc_scope_close(&gC, 0u);
}

/* ---- W2: the stale-entry plant ---- */
static void w2()
{
    for (uint32_t mode = N48_WC_ON; mode <= N48_WC_SHADOW; mode += 2u) {
        pt_reset(); gC = n48_wc {}; gG = n48_wc_gen {};
        map_big(kVmA, 0x400070000ull, 0x33000000ull, 0u);
        WalkCtx wc { &kVmA, 0, nullptr };
        (void)n48_wc_scope_open(&gC, mode, 7u);
        uint32_t m = 0, h = 0;
        (void)look(kVmA, 0x400070000ull, wc, &m, &h);
        map_big(kVmA, 0x400070000ull, 0x34000000ull, 1u);   /* A REMAP PATH THAT FORGETS TO BUMP */
        (void)look(kVmA, 0x400073000ull, wc, &m, &h);
        if (mode == N48_WC_SHADOW) {
            expect_u("W2 SHADOW catches a remap that did not bump: one disagreement counted", gC.st.disagree, 1u);
            expect_u("W2 SHADOW recorded it (VA, cached page, walked page)", gC.ndis == 1u && gC.dis[0].va == 0x400073000ull &&
                     gC.dis[0].cpage == 0x33003000ull && gC.dis[0].wpage == 0x34003000ull && gC.dis[0].wsys == 1u ? 1u : 0u, 1u);
            expect_u("W2 SHADOW USED the walk's answer (no mismatch against the oracle)", m, 0u);
        } else {
            expect_u("W2 ON would have answered STALE (why ON waits on SHADOW's zero on hardware)", m, 1u);
        }
        n48_wc_scope_close(&gC, 0u);
    }
    /* the first N48_WC_DIS only */
    pt_reset(); gC = n48_wc {}; gG = n48_wc_gen {};
    WalkCtx wc { &kVmA, 0, nullptr };
    (void)n48_wc_scope_open(&gC, N48_WC_SHADOW, 7u);
    for (uint32_t i = 0; i < 12u; i++) {
        const uint64_t va = 0x400100000ull + (uint64_t)i * 0x10000ull;
        map_big(kVmA, va, 0x35000000ull + i * 0x10000ull, 0u);
        uint32_t m = 0, h = 0;
        (void)look(kVmA, va, wc, &m, &h);
        map_big(kVmA, va, 0x36000000ull + i * 0x10000ull, 0u);
        (void)look(kVmA, va + 0x1000ull, wc, &m, &h);
    }
    expect_u("W2 12 disagreements counted, only the first 8 recorded", gC.st.disagree == 12u && gC.ndis == N48_WC_DIS ? 1u : 0u, 1u);
    n48_wc_scope_close(&gC, 0u);
}

/* ---- W3: OFF identity ---- */
static uint32_t model_dispatch(const Vm &vm, uint64_t va, uintptr_t self, WalkCtx &wc, uint64_t *walksDirect)
{
    if (__atomic_load_n(&gC.open, __ATOMIC_ACQUIRE) && n48_wc_mine(&gC, self)) {
        uint32_t m = 0, h = 0; return look(vm, va, wc, &m, &h);
    }
    (*walksDirect)++;
    uint64_t p = 0; bool s = false;
    return oracle_walk(vm, va, p, s, nullptr, nullptr) ? 1u : 0u;
}
static void w3()
{
    pt_reset(); gC = n48_wc {}; gG = n48_wc_gen {};
    map_big(kVmA, 0x400080000ull, 0x37000000ull, 0u);
    expect_u("W3 OFF: a scope is never opened", n48_wc_scope_open(&gC, N48_WC_OFF, 7u), 0u);
    expect_u("W3 unknown mode 0 / 4: never opened", n48_wc_scope_open(&gC, 0u, 7u) + n48_wc_scope_open(&gC, 4u, 7u), 0u);
    WalkCtx wc { &kVmA, 0, nullptr };
    uint64_t direct = 0;
    for (uint32_t i = 0; i < 10000u; i++) (void)model_dispatch(kVmA, 0x400080000ull + (i % 16u) * 0x1000ull, 7u, wc, &direct);
    const n48_wc_st z {};
    expect_u("W3 OFF, 10^4 gfxc_page calls: 10^4 direct walks, 0 through the cache", direct == 10000u && wc.walks == 0u ? 1u : 0u, 1u);
    expect_u("W3 OFF: no counter of the cache moved", std::memcmp(&gC.st, &z, sizeof z) == 0 ? 1u : 0u, 1u);
    /* another thread's scope is not mine: its walks go direct */
    (void)n48_wc_scope_open(&gC, N48_WC_ON, 9u);
    direct = 0;
    for (uint32_t i = 0; i < 100u; i++) (void)model_dispatch(kVmA, 0x400080000ull, 7u, wc, &direct);
    expect_u("W3 a scope another thread opened answers none of this thread's walks (the gate's walks run on no scope)", direct, 100u);
    expect_u("W3 a second open while one stands is refused (nested counted)", n48_wc_scope_open(&gC, N48_WC_ON, 9u) == 0u &&
             gC.st.nested == 1u ? 1u : 0u, 1u);
    n48_wc_scope_close(&gC, 0u);
    expect_u("W3 closed: n48_wc_mine is 0 for the old owner", n48_wc_mine(&gC, 9u), 0u);
    int set = 0; uint32_t m = N48_WC_OFF;
    set = n48_wc_set(0u, &m) + n48_wc_set(4u, &m) + n48_wc_set(255u, &m);
    expect_u("W3 the setter refuses M 0 / 4 / 255 and leaves the mode", set == 0 && m == N48_WC_OFF ? 1u : 0u, 1u);
    expect_u("W3 the setter takes 1, 2, 3", (n48_wc_set(1u, &m) && m == 1u && n48_wc_set(3u, &m) && m == 3u && n48_wc_set(2u, &m) &&
             m == 2u) ? 1u : 0u, 1u);
}

/* ---- W4 / W5 / W6 ---- */
static void w456()
{
    pt_reset(); gC = n48_wc {}; gG = n48_wc_gen {};
    WalkCtx wc { &kVmA, 0, nullptr };
    (void)n48_wc_scope_open(&gC, N48_WC_ON, 7u);
    uint32_t m = 0, h = 0;
    const uint32_t ok1 = look(kVmA, 0x400090000ull, wc, &m, &h);
    const uint32_t ok2 = look(kVmA, 0x400090000ull, wc, &m, &h);
    expect_u("W4 a failed walk: both asks fail, neither hits, nothing stored", ok1 == 0u && ok2 == 0u && h == 0u && gC.st.stored == 0u &&
             gC.st.fails == 2u && wc.walks == 2u ? 1u : 0u, 1u);
    /* !ok vm: never answers from an ok vm's entry */
    map_big(kVmA, 0x4000a0000ull, 0x38000000ull, 0u);
    (void)look(kVmA, 0x4000a0000ull, wc, &m, &h);
    Vm bad = kVmA; bad.ok = false;
    const uint32_t okb = look(bad, 0x4000a1000ull, wc, &m, &h);
    expect_u("W4 a vm that is not ok never hits an ok vm's entry (and fails as the walk does)", okb == 0u && h == 0u && m == 0u ? 1u : 0u, 1u);
    /* W6: the 64 KiB granule */
    const uint64_t w0 = wc.walks, h0 = gC.st.hits;
    for (uint32_t p = 0; p < 16u; p++) (void)look(kVmA, 0x4000a0000ull + p * 0x1000ull, wc, &m, &h);
    expect_u("W6 16 pages of a stored 64 KiB leaf: 0 walks, 16 hits", wc.walks - w0 == 0u && gC.st.hits - h0 == 16u ? 1u : 0u, 1u);
    map_big(kVmA, 0x4000b0000ull, 0x39000000ull, 1u);
    const uint64_t w1c = wc.walks;
    uint32_t mis = 0;
    for (uint32_t p = 0; p < 16u; p++) { (void)look(kVmA, 0x4000b0000ull + p * 0x1000ull, wc, &m, &h); mis += m; }
    expect_u("W6 a fresh 64 KiB leaf: 1 walk for its 16 pages, every answer the walk's (SYSTEM bit included)", wc.walks - w1c == 1u &&
             mis == 0u ? 1u : 0u, 1u);
    map_small(kVmA, 0x4000c0000ull, 0x3a000000ull, 0u);
    map_small(kVmA, 0x4000c1000ull, 0x3a100000ull, 0u);
    const uint64_t w2c = wc.walks;
    mis = 0;
    for (uint32_t r = 0; r < 3u; r++) for (uint32_t p = 0; p < 2u; p++) { (void)look(kVmA, 0x4000c0000ull + p * 0x1000ull, wc, &m, &h); mis += m; }
    expect_u("W6 sub-table 4 KiB leaves key by the page: 2 walks for 2 pages asked 3 times each", wc.walks - w2c == 2u && mis == 0u ? 1u : 0u, 1u);
    n48_wc_scope_close(&gC, 0u);
    /* unaligned START: a 64 KiB leaf keys by the page */
    pt_reset(); gC = n48_wc {}; gG = n48_wc_gen {};
    WalkCtx wu { &kVmU, 0, nullptr };
    (void)n48_wc_scope_open(&gC, N48_WC_ON, 7u);
    map_big(kVmU, 0x400010000ull, 0x3b000000ull, 0u);
    mis = 0;
    for (uint32_t p = 0; p < 16u; p++) { (void)look(kVmU, 0x400010000ull + p * 0x1000ull, wu, &m, &h); mis += m; }
    expect_u("W6 START not 64 KiB aligned: every page of a 64 KiB leaf walked once (small keys), answers exact", wu.walks == 16u &&
             mis == 0u ? 1u : 0u, 1u);
    n48_wc_scope_close(&gC, 0u);
    /* W5: scope scoping */
    pt_reset(); gC = n48_wc {}; gG = n48_wc_gen {};
    map_big(kVmA, 0x4000d0000ull, 0x3c000000ull, 0u);
    WalkCtx ws { &kVmA, 0, nullptr };
    (void)n48_wc_scope_open(&gC, N48_WC_ON, 7u);
    (void)look(kVmA, 0x4000d0000ull, ws, &m, &h);
    (void)look(kVmA, 0x4000d1000ull, ws, &m, &h);
    const uint32_t hitIn = h;
    n48_wc_scope_close(&gC, 1u);
    (void)n48_wc_scope_open(&gC, N48_WC_ON, 7u);
    (void)look(kVmA, 0x4000d2000ull, ws, &m, &h);
    expect_u("W5 inside one scope the granule hits; in the NEXT scope (same generation) it does not", hitIn == 1u && h == 0u &&
             ws.walks == 2u ? 1u : 0u, 1u);
    n48_wc_scope_close(&gC, 0u);
    expect_u("W5 scopes / scopes with a hit counted", gC.st.scopes == 2u && gC.st.scopes_hit == 1u ? 1u : 0u, 1u);
    /* the delta helper */
    n48_wc_st a {}, b {}, d {};
    a.hits = 10; a.lookups = 30; b.hits = 4; b.lookups = 40;
    n48_wc_sub(&d, &a, &b);
    expect_u("W5 n48_wc_sub: element-wise, a counter that went backwards reads 0", d.hits == 6u && d.lookups == 0u ? 1u : 0u, 1u);
}

/* ---- W7: the kext glue ---- */
static uint64_t fnv(const std::string &s) { uint64_t h = 0xcbf29ce484222325ull; for (unsigned char c : s) { h ^= c; h *= 0x100000001b3ull; } return h; }
static void w7(const char *ahhPath, const char *bringPath)
{
    const std::string a = slurp(ahhPath), b = slurp(bringPath);
    expect_u("W7 sources read", !a.empty() && !b.empty() ? 1u : 0u, 1u);
    expect_u("W7 OFF at boot: gWc98Mode's initializer is N48_WC_OFF", count(a, "static volatile uint32_t gWc98Mode { N48_WC_OFF };"), 1u);
    expect_u("W7 the verb is gWc98Mode's only writer", count(a, "__atomic_store_n(&gWc98Mode,"), 1u);
    expect_u("W7 the verb asks the mid-arm guard with its own selector", count(a,
             "n48_cm_cont_switch_refused(98u, m == 0u ? 1u : 0u, gXdShot.cont, gXdShot.state)"), 1u);
    expect_u("W7 the guard refuses 98 under a standing continuous arm (not a read)",
             n48_cm_cont_switch_refused(98u, 0u, 1u, (uint32_t)N48_CM_SHOT_ARMED), 1u);
    const std::string gp = body_of(a, "static bool gfxc_page(const GfxcVm &vm, uint64_t va, uint64_t &pageBase, bool &isSys, uint64_t *leafOut) {");
    expect_u("W7 gfxc_page's dispatch: the cache only inside THIS thread's open scope, else 0.0.540's two lines", (
             count(gp, "if (__atomic_load_n(&gWc.open, __ATOMIC_ACQUIRE) && n48_wc_mine(&gWc, (uintptr_t)current_thread()))\n"
                       "        return gfxc_page_wc(vm, va, pageBase, isSys, leafOut);") == 1u &&
             count(gp, "if (pf_on()) return gfxc_page_pf(vm, va, pageBase, isSys, leafOut);") == 1u &&
             count(gp, "return gfxc_page_walk(vm, va, pageBase, isSys, leafOut, nullptr);") == 1u) ? 1u : 0u, 1u);
    expect_u("W7 gfxc_page_wc has exactly one caller (gfxc_page)", count(a, "gfxc_page_wc(vm, va, pageBase, isSys, leafOut)"), 1u);
    expect_u("W7 the scope is declared exactly once in the source (no other function opens one)", count(a, "Wc98AskScope wcs;"), 1u);
    expect_u("W7 n48_wc_scope_open is called in one place (Wc98AskScope)", count(a, "n48_wc_scope_open("), 1u);
    const std::string tk = body_of(a, "static int gfxsrc_desc_tiled_ok(void *ctx, uint64_t va, uint32_t mode, uint32_t elemBytes, const uint32_t *t10, uint32_t *clamp) {");
    expect_u("W7 ... and that one place is the tiled ask (_ok and _okt land there), right after its perf540 scope",
             count(tk, "Pf540Scope pfs(N48_PF_T_CB_TILED, N48_MMT_J_ASK);   // build 0.0.540 (switch 96, T4): one clock pair; OFF one "
                       "load (+ ASK); _ok and _okt both land here\n    Wc98AskScope wcs;"), 1u);
    static const char *const gates[] = {
        "static void gfxsrc_cprov_eval(const GfxcVm &vm)", "static int gfxsrc_desc_dcc_ok(void *ctx",
        "static uint32_t rootwrite_arm_context(VmCtxObs *target, uint64_t targetRoot, bool perform,\n", "static uint64_t hook_unmapVA(",
        "static __attribute__((noinline)) bool gfxc_page_sub(" };
    uint32_t clean = 1u;
    for (const char *g : gates) if (count(body_of(a, g), "Wc98AskScope") != 0u) clean = 0u;
    expect_u("W7 no scope in the gate's walkers (cprov, keystone arm, unmapVA's withdrawal), the dcc ask or gfxc_page_sub", clean, 1u);
    expect_u("W7 the scope's constructor: one load of the switch, open only when not OFF",
             count(a, "        const uint32_t m = __atomic_load_n(&gWc98Mode, __ATOMIC_RELAXED);\n        if (m != N48_WC_OFF) wc98_open(m);"), 1u);
    expect_u("W7 its destructor closes only what it opened", count(a, "~Wc98AskScope() { if (opened_) n48_wc_scope_close(&gWc, gWcAskHit); }"), 1u);
    /* bump sites */
    const std::string un = body_of(a, "static uint64_t hook_unmapVA(void *self, uint64_t va, uint64_t size) {");
    expect_u("W7 hook_unmapVA: the bracket is its FIRST statement (every context, every return)", un.find(
             "typedef uint64_t (*Fn)(void *, uint64_t, uint64_t);\n    // build 0.0.541 (switch 98, gfx_wc98.h): FIRST, for ANY context: a remap "
             "is in flight from here to EVERY return (RAII).\n    const Wc98Remap<N48_WC_B_UNMAP_IN, N48_WC_B_UNMAP_OUT> wcRemap;") !=
             std::string::npos ? 1u : 0u, 1u);
    const std::string mp = body_of(a, "static uint64_t hook_mapVA(void *self, uint64_t va, void *mem, uint64_t a, uint64_t b,");
    expect_u("W7 hook_mapVA: the bracket is its FIRST statement", mp.find("typedef uint64_t (*Fn)(void *, uint64_t, void *, uint64_t, uint64_t, uint64_t);\n"
             "    // build 0.0.541 (switch 98, gfx_wc98.h): FIRST, for ANY context: a remap is in flight from here to EVERY return (RAII).\n"
             "    const Wc98Remap<N48_WC_B_MAP_IN, N48_WC_B_MAP_OUT> wcRemap;") != std::string::npos ? 1u : 0u, 1u);
    expect_u("W7 the bracket: enter at construction, leave at destruction", count(a, "    Wc98Remap() { n48_wc_enter(&gN48WcGen, In); }\n"
             "    ~Wc98Remap() { n48_wc_leave(&gN48WcGen, Out); }"), 1u);
    const std::string wd = "n48_wc_bump(&gN48WcGen, N48_WC_B_KS_WITHDRAW);   // build 0.0.541 (switch 98): the keystone's root[511] clear\n"
                           "            const bool wLo = navi48_vram_write_mm(at, &zero, 1);";
    expect_u("W7 the keystone withdrawal bumps right before its root[511] clear", count(un, wd), 1u);
    expect_u("W7 the keystone re-arm (hook_unmapVA) bumps right before its write", count(un,
             "n48_wc_bump(&gN48WcGen, N48_WC_B_KS_ARM);   // build 0.0.541 (switch 98): the keystone's root[511] re-arm\n"
             "            const bool wHi = navi48_vram_write_mm(at + 4, &hiDw, 1);"), 1u);
    expect_u("W7 every keystone arm (rootwrite_arm_context, all callers) bumps right before its write", count(a,
             "n48_wc_bump(&gN48WcGen, N48_WC_B_KS_ARM);   // build 0.0.541 (switch 98): the keystone's root[511] write (every caller)\n"
             "    const bool wHi = navi48_vram_write_mm(r.at + 4, &hiDw, 1);"), 1u);
    expect_u("W7 every WindowServer binding update notes a moved binding (4 sites)", count(a,
             ", n, complete);\n    wc98_ws_gen_note();"), 4u);
    /* build 0.0.542 (the 0.0.541 review's SHOULD-FIX): hook_releaseVMContext is a BRACKET (in flight to every return), its
     * first statement, and no longer a single bump. */
    const std::string rl = body_of(a, "static uint64_t hook_releaseVMContext(void *self, void *ctx) {");
    expect_u("W7 hook_releaseVMContext: the bracket is its FIRST statement (every return)", (rl.find(
             "static uint64_t hook_releaseVMContext(void *self, void *ctx) {\n    // build 0.0.541 (switch 98): a context (and its "
             "root) goes away. 0.0.542") == 0u &&
             count(rl, "    const Wc98Remap<N48_WC_B_CTX_RELEASE, N48_WC_B_CTX_RELEASE> wcRemap;\n    gVmCtxReleases++;") == 1u &&
             rl.find("wcRemap;") < rl.find("gVmCtxReleases++") && count(rl, "return") >= 1u) ? 1u : 0u, 1u);
    expect_u("W7 hook_releaseVMContext: no stray single bump left", count(rl, "n48_wc_bump(&gN48WcGen"), 0u);
    expect_u("W7 ws-valid bumps right before its context base write", count(a,
             "n48_wc_bump(&gN48WcGen, N48_WC_B_WSV_BASE);   // build 0.0.541 (switch 98): a context base register moves\n"
             "                navi48_reg_write32(gcb + kGcvmCtxBaseLo0 + 2u * v, newLo);"), 1u);
    expect_u("W7 hook_submitCommandBuffer bumps", count(body_of(a, "static uint64_t hook_submitCommandBuffer(void *self, void *info) {"),
             "n48_wc_bump(&gN48WcGen, N48_WC_B_SDMA_SUBMIT);"), 1u);
    const std::string co = body_of(b, "int32_t navi48_cg_open(uint64_t lo, uint64_t hi) {");
    expect_u("W7 navi48_cg_open: enter is its FIRST statement", co.find("{\n\t// build 0.0.541 (gfx_wc98.h): a copy scope opens") !=
             std::string::npos && count(co, "n48_wc_enter(&gN48WcGen, N48_WC_B_CG_OPEN);") == 1u ? 1u : 0u, 1u);
    const std::string cc = body_of(b, "void navi48_cg_close(int32_t slot, uint64_t lo, uint64_t hi, bool wrote, bool failed, bool mismatch) {");
    expect_u("W7 navi48_cg_close: the copy-guard END leaves on BOTH paths (untracked return, tracked end)", count(cc,
             "n48_wc_leave(&gN48WcGen, N48_WC_B_CG_END);"), 2u);
    expect_u("W7 ... the untracked path leaves right before its return", count(cc, "n48_wc_leave(&gN48WcGen, N48_WC_B_CG_END);   // build "
             "0.0.541: the copy-guard END, after the untracked count falls\n\t\treturn;"), 1u);
    const std::string wm = body_of(b, "bool navi48_vram_write_mm(uint64_t vramOffset, const uint32_t *src, uint32_t dwords) {");
    expect_u("W7 every MM-window write is inside a copy scope: a write not contained in this thread's scope opens its own",
             (count(wm, "cgSelfSlot = navi48_cg_open(vramOffset, vramOffset + bytes);") == 1u &&
              count(wm, "if (cgSelfScoped) navi48_cg_close(cgSelfSlot, vramOffset, vramOffset + bytes, /*wrote=*/true, /*failed=*/!ok, /*mismatch=*/false);") == 1u)
             ? 1u : 0u, 1u);
    expect_u("W7 the generation's one definition", count(b, "n48_wc_gen gN48WcGen {};"), 1u);
    /* build 0.0.542 (the 0.0.541 review's SHOULD-FIX): its own cache lines - aligned, and the type padded to whole lines */
    expect_u("W7 the generation is alignas(64)", count(b, "alignas(64) n48_wc_gen gN48WcGen {};"), 1u);
    expect_u("W7 n48_wc_gen is whole 64-byte lines", sizeof(n48_wc_gen) % 64u == 0u && sizeof(n48_wc_gen) >= 64u ? 1u : 0u, 1u);
    /* gfxc_page_walk unchanged since 0.0.540 (2c91d368): FNV-1a of its body text */
    const std::string gw = body_of(a, "static inline bool gfxc_page_walk(const GfxcVm &vm, uint64_t va, uint64_t &pageBase, bool &isSys, uint64_t *leafOut, uint32_t *lv) {");
    expect_u("W7 gfxc_page_walk's body is 0.0.540's (FNV-1a pinned)", fnv(gw), 0xe5f237774dcd4294ull);
    expect_u("W7 the report prints at the verb and at the CONTINUOUS STOP only (nothing at the STOP while OFF)",
             (count(a, "wc98_arm_stop();                  // build 0.0.541 (switch 98)") == 1u &&
              count(a, "    if (m == N48_WC_OFF) return;") >= 1u) ? 1u : 0u, 1u);
}

/* ---- W8: the lines ---- */
static void w8()
{
    char buf[2048];
    n48_wc_st s; std::memset(&s, 0xff, sizeof s);
    n48_wc_gen g; std::memset(&g, 0xff, sizeof g); g.out = 0ull;
    uint32_t ok = 1u;
    const char *how = " - `gfxneuter 98` CHANGED BY THIS VERB (counters reset)";
    for (uint32_t m = 1u; m <= 3u; m++) {
        const int n = std::snprintf(buf, sizeof buf, N48_WC_FMT, N48_WC_ARGS(m, how, &s));
        if (n < 0 || (uint32_t)n > N48_LOG_CAP_BODY) { ok = 0u; std::printf("  WC line %d bytes\n", n); }
    }
    const int ng = std::snprintf(buf, sizeof buf, N48_WC_GEN_FMT, N48_WC_GEN_ARGS(&g));
    if (ng < 0 || (uint32_t)ng > N48_LOG_CAP_BODY) { ok = 0u; std::printf("  GEN line %d bytes\n", ng); }
    n48_wc_dis d; std::memset(&d, 0xff, sizeof d); d.wok = 0u;
    const int nd = std::snprintf(buf, sizeof buf, N48_WC_DIS_FMT, N48_WC_DIS_ARGS(7u, &d));
    if (nd < 0 || (uint32_t)nd > N48_LOG_CAP_BODY) { ok = 0u; std::printf("  DIS line %d bytes\n", nd); }
    expect_u("W8 every walkcache98 line fits 491 bytes at maximal fields", ok, 1u);
}


/* ---- W9: build 0.0.543 item B, the instability census ---- */
static void w9(const char *ahh)
{
    /* (a) no behaviour change: the same 40 schedules, census OFF then ON */
    uint32_t diff = 0u;
    uint64_t unstTot = 0ull, attrTot = 0ull;
    for (uint32_t run = 0; run < 40u; run++) {
        const uint64_t seed = gRs;
        uint64_t m0 = 0, d0 = 0, h0 = 0, m1 = 0, d1 = 0, h1 = 0;
        const uint32_t mode = run & 2u ? N48_WC_ON : N48_WC_SHADOW;
        gCensusRun = 0u;
        w1_run(mode, run & 1u ? kVmU : kVmA, 4000u, &m0, &d0, &h0);
        static n48_wc off; off = gC;
        gRs = seed;
        gCensusRun = 1u;
        w1_run(mode, run & 1u ? kVmU : kVmA, 4000u, &m1, &d1, &h1);
        gCensusRun = 0u;
        if (m0 != m1 || d0 != d1 || h0 != h1 || std::memcmp(&off.st, &gC.st, sizeof off.st) != 0 ||
            std::memcmp(off.e, gC.e, sizeof off.e) != 0 || off.ndis != gC.ndis) diff++;
        if (off.cen.unstable != 0u) diff++;   /* OFF counted nothing */
        unstTot += gC.st.unstable;
        attrTot += gC.cen.unstable;
    }
    expect_u("W9 census ON vs OFF over 40 schedules: identical answers, counters and cache contents (runs that differ)", diff, 0u);
    expect_u("W9   every unstable walk was attributed once (census unstable == 98's unstable), and there were some",
             unstTot == attrTot && unstTot > 100u ? 1u : 0u, 1u);

    /* (b) attribution, one scenario each */
    struct Sc { const char *name; uint32_t cls; uint32_t inflight; };
    WalkCtx wc { &kVmA, 0, nullptr };
    auto fresh = [&]() { pt_reset(); gC = n48_wc {}; gG = n48_wc_gen {}; gC.census = 1u; map_big(kVmA, 0x400080000ull, 0x34000000ull, 0u);
                         (void)n48_wc_scope_open(&gC, N48_WC_ON, 7u); };
    uint32_t m = 0, h = 0;
    fresh();
    wc.mid = []() { n48_wc_enter(&gG, N48_WC_B_MAP_IN); n48_wc_leave(&gG, N48_WC_B_MAP_OUT); };
    (void)look(kVmA, 0x400080000ull, wc, &m, &h);
    expect_u("W9 a mapVA bracket completing inside the walk: mapVA MOVED (not in flight), last_m = mapVA",
             gC.cen.unstable == 1u && gC.cen.moved[N48_WC_K_MAP] == 1u && gC.cen.inflight[N48_WC_K_MAP] == 0u &&
             gC.cen.any[N48_WC_K_MAP] == 1u && gC.last_m == (1u << N48_WC_K_MAP), 1u);
    n48_wc_scope_close(&gC, 0u);
    fresh();
    n48_wc_enter(&gG, N48_WC_B_UNMAP_IN);
    (void)look(kVmA, 0x400080000ull, wc, &m, &h);
    expect_u("W9 a lookup while unmapVA is in flight: unmapVA IN FLIGHT", gC.cen.inflight[N48_WC_K_UNMAP] == 1u && gC.cen.any[N48_WC_K_UNMAP] == 1u &&
             gC.cen.none == 0u && gC.last_m == (1u << N48_WC_K_UNMAP), 1u);
    n48_wc_leave(&gG, N48_WC_B_UNMAP_OUT);
    n48_wc_scope_close(&gC, 0u);
    fresh();
    n48_wc_enter(&gG, N48_WC_B_CG_OPEN);
    (void)look(kVmA, 0x400080000ull, wc, &m, &h);
    expect_u("W9 a lookup inside a copy scope: copy guard IN FLIGHT", gC.cen.inflight[N48_WC_K_CG] == 1u && gC.last_m == (1u << N48_WC_K_CG), 1u);
    n48_wc_leave(&gG, N48_WC_B_CG_END);
    n48_wc_scope_close(&gC, 0u);
    fresh();
    wc.mid = []() { n48_wc_bump(&gG, N48_WC_B_KS_ARM); };
    (void)look(kVmA, 0x400080000ull, wc, &m, &h);
    expect_u("W9 a keystone bump inside the walk: OTHER moved", gC.cen.moved[N48_WC_K_OTHER] == 1u && gC.last_m == (1u << N48_WC_K_OTHER), 1u);
    n48_wc_scope_close(&gC, 0u);
    fresh();
    n48_wc_enter(&gG, N48_WC_B_CTX_RELEASE);
    (void)look(kVmA, 0x400080000ull, wc, &m, &h);
    expect_u("W9 a ctx release in flight (one site for both ends): OTHER in flight", gC.cen.inflight[N48_WC_K_OTHER] == 1u, 1u);
    n48_wc_leave(&gG, N48_WC_B_CTX_RELEASE);
    n48_wc_scope_close(&gC, 0u);
    fresh();
    n48_wc_enter(&gG, N48_WC_B_MAP_IN);
    n48_wc_enter(&gG, N48_WC_B_CG_OPEN);
    (void)look(kVmA, 0x400080000ull, wc, &m, &h);
    expect_u("W9 two classes in flight: both counted, multi 1", gC.cen.any[N48_WC_K_MAP] == 1u && gC.cen.any[N48_WC_K_CG] == 1u &&
             gC.cen.multi == 1u && gC.cen.unstable == 1u, 1u);
    n48_wc_leave(&gG, N48_WC_B_CG_END);
    n48_wc_leave(&gG, N48_WC_B_MAP_OUT);
    n48_wc_scope_close(&gC, 0u);
    uint64_t s0[N48_WC_B_N] = {}, s1[N48_WC_B_N] = {};
    uint32_t fl = 9u;
    expect_u("W9 nothing attributable (same snapshots, both views stable): NONE", n48_wc_attr(s0, s1, 1u, 1u, &fl) == N48_WC_K_NONE_BIT &&
             fl == 0u, 1u);
    n48_wc_cen cc {};
    n48_wc_cen_note(&cc, N48_WC_K_NONE_BIT, 0u);
    expect_u("W9   counted as none", cc.none == 1u && cc.unstable == 1u, 1u);
    /* census OFF: nothing written */
    pt_reset(); gC = n48_wc {}; gG = n48_wc_gen {}; map_big(kVmA, 0x400080000ull, 0x34000000ull, 0u);
    (void)n48_wc_scope_open(&gC, N48_WC_ON, 7u);
    gC.last_m = 0x55u;
    n48_wc_enter(&gG, N48_WC_B_UNMAP_IN);
    (void)look(kVmA, 0x400080000ull, wc, &m, &h);
    n48_wc_leave(&gG, N48_WC_B_UNMAP_OUT);
    expect_u("W9 census OFF: the unstable walk is 98's as before, the census writes nothing (last_m untouched)",
             gC.st.unstable == 1u && gC.cen.unstable == 0u && gC.last_m == 0x55u, 1u);
    n48_wc_scope_close(&gC, 0u);
    uint32_t on = 7u;
    expect_u("W9 the setter: 1 ON, 2 OFF, others refused unchanged", n48_wc_cen_set(1u, &on) == 1 && on == 1u && n48_wc_cen_set(2u, &on) == 1 &&
             on == 0u && n48_wc_cen_set(3u, &on) == 0 && n48_wc_cen_set(0u, &on) == 0 && on == 0u, 1u);
    expect_u("W9 101 is mid-arm guarded (reads allowed)", n48_cm_cont_switch_refused(101u, 0u, 1u, (uint32_t)N48_CM_SHOT_ARMED) == 1u &&
             n48_cm_cont_switch_refused(101u, 1u, 1u, (uint32_t)N48_CM_SHOT_ARMED) == 0u, 1u);

    /* (c) the lines */
    char b[2048];
    n48_wc_cen mx; std::memset(&mx, 0xff, sizeof mx);
    n48_wc_gen gx; std::memset(&gx, 0, sizeof gx); gx.site[N48_WC_B_MAP_IN] = ~0ull; gx.site[N48_WC_B_UNMAP_IN] = ~0ull; gx.site[N48_WC_B_CG_OPEN] = ~0ull;
    uint32_t ok = 1u;
    const char *hows[] = { " - `gfxneuter 101` REFUSED - a continuous arm stands, unchanged", " - `gfxneuter 101` CHANGED BY THIS VERB (counters reset)",
                           " - `gfxneuter 101` REFUSED (unknown M), unchanged", " - CONTINUOUS STOP: this arm's deltas" };
    for (const char *hw : hows) {
        const int n = std::snprintf(b, sizeof b, N48_WC_CEN_FMT, N48_WC_CEN_ARGS(0u, hw, &mx));
        if (n < 0 || n > 491) { ok = 0u; std::printf("  too long (%d): %s\n", n, b); }
        const int n1 = std::snprintf(b, sizeof b, N48_WC_CEN_FMT2, N48_WC_CEN_ARGS2(&mx, &gx));
        if (n1 < 0 || n1 > 491) { ok = 0u; std::printf("  too long (%d): %s\n", n1, b); }
    }
    n48_pf_secline L; for (uint32_t k = 0; k < N48_PF_S_N; k++) L.v[k] = ~0ull;
    L.line = 4294967295u; L.at_ms = ~0ull; L.len_ms = ~0ull;
    const int n2 = std::snprintf(b, sizeof b, N48_PF_WCU_SEC_FMT, N48_PF_WCU_SEC_ARGS(L));
    if (n2 < 0 || n2 > 491) { ok = 0u; std::printf("  too long (%d): %s\n", n2, b); }
    expect_u("W9 the census line and the per-second census line fit 491 bytes at maximal fields", ok, 1u);

    /* (d) the kext glue */
    if (!ahh) { expect_u("W9 needs AppleHardwareHook.cpp", 0u, 1u); return; }
    const std::string s = slurp(ahh);
    expect_u("W9 kext: the 101 verb is gWc.census's only writer", count(s, "__atomic_store_n(&gWc.census,") == 1u &&
             count(s, "gWc.census =") == 0u, 1u);
    expect_u("W9 kext: the 101 verb asks the mid-arm guard", count(s, "n48_cm_cont_switch_refused(101u, m == 0u ? 1u : 0u, gXdShot.cont, gXdShot.state)"), 1u);
    const std::string pw = body_of(s, "static __attribute__((noinline)) bool gfxc_page_wc(");
    expect_u("W9 kext: gfxc_page_wc counts the walk's attribution right after perf540's two counters",
             count(pw, "    if (pf_on()) { pf_count(N48_PF_C_WC_LOOK, 1u); if (hit) pf_count(N48_PF_C_WC_HIT, 1u); }\n"
                       "    if (gWc.census && gWc.last_m) wc101_pf(gWc.last_m);") == 1u, 1u);
    expect_u("W9 kext: wc101_pf counts only while perf540 is ON", count(s, "static __attribute__((noinline)) void wc101_pf(uint32_t m)\n{\n    if (!pf_on()) return;"), 1u);
    expect_u("W9 kext: the census STOP line follows 98's STOP lines, deltas against the START snapshot",
             count(s, "    HWLOG(N48_WC_GEN_FMT, N48_WC_GEN_ARGS(&gN48WcGen));\n    // build 0.0.543 item B (switch 101)") == 1u &&
             count(s, "n48_wc_cen_sub(&dc, &gWc.cen, gWcArmSnapValid ? &gWcCenSnap : &zc);") == 1u &&
             count(s, "    gWcCenSnap = gWc.cen;   // build 0.0.543 item B") == 1u, 1u);
    expect_u("W9 kext: the per-second census line follows perf540's sec line of the same window",
             count(s, "if (n48_pf_win_close(&gPfWin, &v, ns / 1000ull, &L)) HWLOG(N48_PF_SEC_FMT, N48_PF_SEC_ARGS(L));\n"
                      "        // build 0.0.543 item B (switch 101): the same window's census, right after its sec line (census OFF: nothing).\n"
                      "        if (L.line && __atomic_load_n(&gWc.census, __ATOMIC_RELAXED)) HWLOG(N48_PF_WCU_SEC_FMT, N48_PF_WCU_SEC_ARGS(L));") == 1u, 1u);
}

int main(int argc, char **argv)
{
    w1(); w2(); w3(); w456();
    w7(argc > 1 ? argv[1] : nullptr, argc > 2 ? argv[2] : nullptr);
    w8();
    w9(argc > 1 ? argv[1] : nullptr);   // build 0.0.543 item B
    std::printf("%s: %d run, %d failed\n", gFail ? "FAIL" : "PASS", gRun, gFail);
    return gFail ? 1 : 0;
}
