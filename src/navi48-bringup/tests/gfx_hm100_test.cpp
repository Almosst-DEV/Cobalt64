// gfx_hm100_test.cpp — build 0.0.543 item A ( ranked change A; switch 100, gfx_hm100.h): the per-call host-page map
// cache.
//     clang++ -std=c++17 -Wall -Wextra -Werror -O1 -fsanitize=address,undefined -fno-sanitize-recover=all \
//         -I src/navi48-bringup/src/apple src/navi48-bringup/tests/gfx_hm100_test.cpp -o /tmp/hm100 && \
//         /tmp/hm100 src/navi48-bringup/src/apple/AppleHardwareHook.cpp
// Covers:
//   H1 the table: open / nested / owner; N48_HM_N kept, the next one FULL (used once, released at once); close releases every kept
//      map exactly once, in order, and publishes closed; a live-map ledger (fake IOKit) reads 0 after every call;
//   H2 EVERY EXIT RELEASES: a model call with an RAII scope (Hm100Scope's shape) and 12 exit points, randomized; the kext's scope is
//      the FIRST statement of hook_gfxCommitIB_timed, its destructor closes with hm100_rel, and hm100_rel releases the map, completes
//      and releases the descriptor (source pins);
//   H3 NO NON-OWNER USE: another thread's access never looks up, inserts or closes; its map is released after its one use; the kext
//      takes the cached path only on hm100_mine() (the scope open AND owner == current_thread());
//   H4 THE GUARD ON EVERY ACCESS: a guard-refused page is never looked up, mapped or kept - even when it is already in the table;
//      in the kext the walk, then n48_rt_refuse, then hm100_mine() run in that order in both gfxc_read_core and gfxc_write_sys, and
//      the cached path hands n48_hm_get the guard's verdict again;
//   H5 A VA REMAPPED MID-CALL READS THE NEW PAGE: a model of gfxc_read_core's loop (walk -> guard -> n48_hm_get) over fake physical
//      memory, a randomized remap / write / guard-bound schedule within calls, ON and SHADOW against an uncached oracle: 0 mismatches;
//   H6 keyed by the PHYSICAL page: two VAs aliasing one page share one entry; the kext passes `page`, never the VA (pins);
//   H7 direction: every map the owner keeps is OUTIN; an IN insert is refused; every write goes through an OUTIN map;
//   H8 SHADOW uses the fresh map and counts a planted stale cached page (read and write); ON never maps on a hit;
//   H9 OFF identity: OFF never opens; the verb is gHm100Mode's only writer, boot OFF, mid-arm guarded; 0.0.542's uncached lines are
//      intact in the else branches;
//   H10 every hostmap100 line <= 491 bytes at maximal fields.
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <map>
#include <vector>
#include <random>
#include "gfx_hm100.h"
#include "gfx_commit.h"

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
    FILE *f = std::fopen(p, "rb");
    if (!f) return std::string();
    std::string s;
    char buf[65536];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) s.append(buf, n);
    std::fclose(f);
    return s;
}
static uint32_t count(const std::string &s, const char *x)
{
    uint32_t c = 0u;
    for (size_t p = s.find(x); p != std::string::npos; p = s.find(x, p + 1)) c++;
    return c;
}
static std::string body_from(const std::string &s, const char *start, const char *end)
{
    const size_t a = s.find(start);
    if (a == std::string::npos) return std::string();
    const size_t b = s.find(end, a + std::strlen(start));
    return b == std::string::npos ? std::string() : s.substr(a, b - a);
}
static bool ordered(const std::string &s, std::initializer_list<const char *> xs)
{
    size_t at = 0;
    for (const char *x : xs) {
        const size_t p = s.find(x, at);
        if (p == std::string::npos) return false;
        at = p + std::strlen(x);
    }
    return true;
}

/* ---- a fake IOKit: physical pages are 1024-dword buffers; a map's kva is the page buffer itself (every map of a page aliases the
 * same bytes, as on the machine); a live-map ledger counts every mk not yet released. A page in `stale` maps to a private copy (the
 * SHADOW plant: a cached map that no longer shows the page). ---- */
struct FakeMem {
    std::map<uint64_t, std::vector<uint32_t>> phys;
    std::map<uint64_t, std::vector<uint32_t>> stale;
    uint64_t live = 0, made = 0, released = 0, failNext = 0;
    bool staleOff = false;   /* later maps show the real page; the stale copy stays allocated (a kept map still points at it) */
    std::vector<uint32_t> dirs;   /* every mk's direction */
    std::vector<uint32_t> &page(uint64_t p) { auto &v = phys[p]; if (v.empty()) v.assign(1024, (uint32_t)(p >> 12) * 0x10001u); return v; }
};
static FakeMem *gM = nullptr;
struct FakeMap { uint64_t page; uint32_t dir; int alive; };
static uint32_t fake_mk(void *ctx, uint64_t page, uint32_t dir, void **md, void **map, uint64_t *kva)
{
    FakeMem *m = static_cast<FakeMem *>(ctx);
    if (m->failNext) { m->failNext--; return 0u; }
    FakeMap *fm = new FakeMap { page, dir, 1 };
    *md = fm; *map = fm;
    auto st = m->stale.find(page);
    *kva = (uint64_t)(uintptr_t)(st != m->stale.end() && !m->staleOff ? st->second.data() : m->page(page).data());
    m->live++; m->made++;
    m->dirs.push_back(dir);
    return 1u;
}
static void fake_rel(void *ctx, void *md, void *map, uint32_t dir)
{
    FakeMem *m = ctx ? static_cast<FakeMem *>(ctx) : gM;
    FakeMap *fm = static_cast<FakeMap *>(md);
    if (!fm || fm != map || !fm->alive || fm->dir != dir) { std::printf("  BAD RELEASE\n"); gFail++; return; }
    fm->alive = 0;
    delete fm;
    m->live--; m->released++;
}

/* The model of gfxc_read_core / gfxc_write_sys's host-page step (walk -> guard -> cached path), one page. */
struct Model {
    n48_hm *c; FakeMem *m; uintptr_t self;
    std::map<uint64_t, uint64_t> pt;   /* VA page -> physical page */
    uint64_t ramTop = 1ull << 40;
    uint32_t guard(uint64_t page) const { return page >= ramTop ? 1u : 0u; }
    bool read(uint64_t va, uint32_t *dst, uint32_t k, uint32_t *w = nullptr) {
        auto it = pt.find(va & ~0xfffull);
        if (it == pt.end()) return false;              /* the walk */
        const uint64_t page = it->second;
        if (guard(page)) return false;                 /* the guard (the kext's own `if (rr) break;`) */
        n48_hm_use u {};
        if (!n48_hm_get(c, self, page, guard(page), 0u, N48_HM_DIR_IN, &fake_mk, m, &u)) return false;
        const volatile uint32_t *s = (const volatile uint32_t *)(uintptr_t)(u.kva + (va & 0xfffu));
        for (uint32_t i = 0; i < k; i++) dst[i] = s[i];
        if (u.shadow_kva) (void)n48_hm_compare(c, page, (uint32_t)(va & 0xfffu), dst, u.shadow_kva, k, 0u);
        if (w) *w = u.dir;
        if (u.rel) fake_rel(m, u.md, u.map, u.dir);
        return true;
    }
    bool write(uint64_t va, const uint32_t *src, uint32_t k, uint32_t *wdir) {
        auto it = pt.find(va & ~0xfffull);
        if (it == pt.end()) return false;
        const uint64_t page = it->second;
        if (guard(page)) return false;
        n48_hm_use u {};
        if (!n48_hm_get(c, self, page, guard(page), 1u, N48_HM_DIR_OUTIN, &fake_mk, m, &u)) return false;
        *wdir = u.dir;
        volatile uint32_t *d = (volatile uint32_t *)(uintptr_t)(u.kva + (va & 0xfffu));
        for (uint32_t i = 0; i < k; i++) d[i] = src[i];
        if (u.shadow_kva) (void)n48_hm_compare(c, page, (uint32_t)(va & 0xfffu), src, u.shadow_kva, k, 1u);
        if (u.rel) fake_rel(m, u.md, u.map, u.dir);
        return true;
    }
};
/* Hm100Scope's shape. */
struct TScope {
    n48_hm *c; FakeMem *m; uintptr_t self; uint32_t opened;
    TScope(n48_hm *c_, FakeMem *m_, uint32_t mode, uintptr_t s) : c(c_), m(m_), self(s), opened(n48_hm_open(c_, mode, s)) {}
    ~TScope() { if (opened) (void)n48_hm_close(c, self, &fake_rel, m); }
};

static void h1()
{
    static n48_hm c {};
    FakeMem m; gM = &m;
    expect_u("H1 OFF never opens", n48_hm_open(&c, N48_HM_OFF, 1u), 0u);
    expect_u("H1 an unknown mode never opens", n48_hm_open(&c, 7u, 1u), 0u);
    expect_u("H1 ON opens for owner 1", n48_hm_open(&c, N48_HM_ON, 1u), 1u);
    expect_u("H1 a second open (another thread) is refused and counted nested", n48_hm_open(&c, N48_HM_ON, 2u) == 0u && c.st.nested == 1u, 1u);
    expect_u("H1 mine for 1, not for 2", n48_hm_mine(&c, 1u) * 10u + n48_hm_mine(&c, 2u), 10u);
    uint32_t okAll = 1u;
    for (uint32_t i = 0; i < N48_HM_N; i++) {
        n48_hm_use u {};
        if (!n48_hm_get(&c, 1u, 0x100000ull + (uint64_t)i * 4096ull, 0u, 0u, N48_HM_DIR_IN, &fake_mk, &m, &u) || u.rel) okAll = 0u;
    }
    expect_u("H1 128 distinct pages are all kept (rel 0)", okAll, 1u);
    n48_hm_use u {};
    expect_u("H1 the 129th is mapped, NOT kept (rel 1), counted full",
             n48_hm_get(&c, 1u, 0x900000ull, 0u, 0u, N48_HM_DIR_IN, &fake_mk, &m, &u) == 1u && u.rel == 1u && c.st.full == 1u, 1u);
    fake_rel(&m, u.md, u.map, u.dir);
    n48_hm_use h {};
    expect_u("H1 a kept page hits with no new map (ON)", n48_hm_get(&c, 1u, 0x100000ull, 0u, 0u, N48_HM_DIR_IN, &fake_mk, &m, &h) == 1u &&
             h.rel == 0u && h.md == nullptr && c.st.hits == 1u && m.made == 129u, 1u);
    expect_u("H1 live maps before close = 128", m.live, 128u);
    expect_u("H1 a non-owner close is refused and releases nothing", n48_hm_close(&c, 2u, &fake_rel, &m) == 0u && m.live == 128u, 1u);
    expect_u("H1 the owner's close releases all 128", n48_hm_close(&c, 1u, &fake_rel, &m), 128u);
    expect_u("H1 live maps after close = 0; released == made", m.live == 0u && m.released == m.made, 1u);
    expect_u("H1 closed: nobody's, and a new open works", n48_hm_mine(&c, 1u) == 0u && c.open == 0u && n48_hm_open(&c, N48_HM_SHADOW, 3u) == 1u, 1u);
    (void)n48_hm_close(&c, 3u, &fake_rel, &m);
    m.failNext = 1u;
    (void)n48_hm_open(&c, N48_HM_ON, 1u);
    n48_hm_use f {};
    expect_u("H1 a failed map is refused, keeps nothing, counted", n48_hm_get(&c, 1u, 0x5000ull, 0u, 0u, N48_HM_DIR_IN, &fake_mk, &m, &f) == 0u &&
             c.n == 0u && c.st.mapfail == 1u, 1u);
    (void)n48_hm_close(&c, 1u, &fake_rel, &m);
}

static void h2(const std::string &s)
{
    static n48_hm c {};
    FakeMem m; gM = &m;
    std::mt19937 rng(543u);
    uint32_t leaks = 0u, calls = 0u;
    for (uint32_t call = 0; call < 2000u; call++) {
        const uint32_t exitAt = rng() % 12u;
        const uint32_t mode = (call & 1u) ? N48_HM_ON : N48_HM_SHADOW;
        [&]() {
            TScope sc(&c, &m, mode, 1u);
            Model md { &c, &m, 1u, {}, 1ull << 40 };
            for (uint32_t k = 0; k < 16u; k++) md.pt[0x400000000ull + k * 4096ull] = 0x10000000ull + (rng() % 40u) * 4096ull;
            for (uint32_t step = 0; step < 12u; step++) {
                if (step == exitAt) return;                 /* an early return: the scope's destructor must release */
                uint32_t d[8];
                (void)md.read(0x400000000ull + (rng() % 16u) * 4096ull + (rng() % 1000u) * 4u, d, 1u);
            }
        }();
        calls++;
        if (m.live) { leaks++; m.live = 0; }
        if (c.open) leaks++;
    }
    expect_u("H2 2000 model calls with random early exits: live maps 0 and the scope closed after every one", leaks + (calls == 2000u ? 0u : 1000u), 0u);
    expect_u("H2   released == made", m.released == m.made && m.made > 2000u, 1u);
    if (s.empty()) return;
    const std::string w = body_from(s, "static uint64_t hook_gfxCommitIB_timed(void *self, void *info) {", "\n}\n");
    expect_u("H2 kext: Hm100Scope is hook_gfxCommitIB_timed's FIRST statement",
             w.find("static uint64_t hook_gfxCommitIB_timed(void *self, void *info) {\n    const Hm100Scope hmScope;") == 0u ? 1u : 0u, 1u);
    expect_u("H2 kext: the scope is declared nowhere else", count(s, "const Hm100Scope hmScope;"), 1u);
    expect_u("H2 kext: the destructor closes with hm100_rel when it opened",
             count(s, "~Hm100Scope() { if (opened_) hm100_close(); }") == 1u &&
             count(s, "static void hm100_close() { (void)n48_hm_close(&gHm, (uintptr_t)current_thread(), &hm100_rel, nullptr); }") == 1u, 1u);
    const std::string rel = body_from(s, "static void hm100_rel(void *, void *mdv, void *mapv, uint32_t dir)", "\n}\n");
    expect_u("H2 kext: hm100_rel releases the map, then completes and releases the descriptor",
             ordered(rel, { "if (map) map->release();", "if (md) { md->complete(d); md->release(); }" }) ? 1u : 0u, 1u);
    const std::string mk = body_from(s, "static uint32_t hm100_mk(void *, uint64_t page, uint32_t dir, void **mdOut, void **mapOut, uint64_t *kva)", "\n}\n");
    expect_u("H2 kext: hm100_mk's failure paths release what they made",
             count(mk, "if (md->prepare(d) != kIOReturnSuccess) { md->release(); return 0u; }") == 1u &&
             count(mk, "if (!map) { md->complete(d); md->release(); return 0u; }") == 1u, 1u);
    const std::string rd = body_from(s, "static __attribute__((noinline)) bool hm100_host_read(", "\n}\n");
    const std::string wr = body_from(s, "static __attribute__((noinline)) bool hm100_host_write(", "\n}\n");
    expect_u("H2 kext: the read and the write release an uncached map after its one use",
             count(rd, "if (u.rel) hm100_rel(nullptr, u.md, u.map, u.dir);") == 1u && count(wr, "if (u.rel) hm100_rel(nullptr, u.md, u.map, u.dir);") == 1u, 1u);
}

static void h3(const std::string &s)
{
    static n48_hm c {};
    FakeMem m; gM = &m;
    (void)n48_hm_open(&c, N48_HM_ON, 1u);
    n48_hm_use a {};
    (void)n48_hm_get(&c, 1u, 0x7000ull, 0u, 0u, N48_HM_DIR_IN, &fake_mk, &m, &a);
    const uint32_t n0 = c.n; const uint64_t look0 = c.st.lookups, hits0 = c.st.hits;
    n48_hm_use b {};
    expect_u("H3 another thread's access to a KEPT page is served uncached (a fresh map, rel 1, no hit)",
             n48_hm_get(&c, 2u, 0x7000ull, 0u, 0u, N48_HM_DIR_IN, &fake_mk, &m, &b) == 1u && b.rel == 1u && b.md != nullptr &&
             c.st.hits == hits0 && c.st.lookups == look0, 1u);
    expect_u("H3   its direction is the caller's own (0.0.542's kIODirectionIn for a read)", b.dir, (uint64_t)N48_HM_DIR_IN);
    fake_rel(&m, b.md, b.map, b.dir);
    n48_hm_use b2 {};
    (void)n48_hm_get(&c, 2u, 0x8000ull, 0u, 0u, N48_HM_DIR_IN, &fake_mk, &m, &b2);
    expect_u("H3 another thread's miss inserts nothing", c.n == n0 && b2.rel == 1u && c.st.notowner == 2u, 1u);
    fake_rel(&m, b2.md, b2.map, b2.dir);
    expect_u("H3 another thread cannot close the owner's scope", n48_hm_close(&c, 2u, &fake_rel, &m) == 0u && c.open == 1u, 1u);
    (void)n48_hm_close(&c, 1u, &fake_rel, &m);
    expect_u("H3 live maps 0", m.live, 0u);
    if (s.empty()) return;
    expect_u("H3 kext: hm100_mine() = the scope open AND n48_hm_mine(&gHm, current_thread())",
             count(s, "return __atomic_load_n(&gHm.open, __ATOMIC_ACQUIRE) == 1u && n48_hm_mine(&gHm, (uintptr_t)current_thread()) != 0u;"), 1u);
    expect_u("H3 kext: the cached read / write are reached only through `if (hm100_mine())`",
             count(s, "hm100_host_read(") == 2u && count(s, "hm100_host_write(") == 2u &&
             count(s, "if (hm100_mine()) ok = hm100_host_read(page, off, &dst[got], k);") == 1u &&
             count(s, "if (hm100_mine()) ok = hm100_host_write(page, off, &src[put], k);") == 1u, 1u);
    expect_u("H3 kext: every n48_hm_get passes current_thread() as the caller",
             count(s, "n48_hm_get(&gHm, (uintptr_t)current_thread(), page,") == 2u && count(s, "n48_hm_get(") == 2u, 1u);
    expect_u("H3 kext: gHm is opened only by Hm100Scope", count(s, "n48_hm_open(&gHm,") == 1u, 1u);
}

static void h4(const std::string &s)
{
    static n48_hm c {};
    FakeMem m; gM = &m;
    (void)n48_hm_open(&c, N48_HM_ON, 1u);
    n48_hm_use a {};
    (void)n48_hm_get(&c, 1u, 0x9000ull, 0u, 0u, N48_HM_DIR_IN, &fake_mk, &m, &a);
    const uint64_t made0 = m.made, look0 = c.st.lookups;
    n48_hm_use g {};
    expect_u("H4 a guard-refused page is refused even though it is KEPT (no hit, no lookup, no map)",
             n48_hm_get(&c, 1u, 0x9000ull, 1u, 0u, N48_HM_DIR_IN, &fake_mk, &m, &g) == 0u && g.kva == 0ull && m.made == made0 &&
             c.st.lookups == look0 && c.st.guard == 1u, 1u);
    n48_hm_use g2 {};
    expect_u("H4 a guard-refused new page is never mapped or kept",
             n48_hm_get(&c, 1u, 0xA000ull, 2u, 1u, N48_HM_DIR_OUTIN, &fake_mk, &m, &g2) == 0u && m.made == made0 && c.n == 1u, 1u);
    n48_hm_use g3 {};
    expect_u("H4 a guard-refused page is refused for another thread too (no map)",
             n48_hm_get(&c, 2u, 0xB000ull, 1u, 0u, N48_HM_DIR_IN, &fake_mk, &m, &g3) == 0u && m.made == made0, 1u);
    (void)n48_hm_close(&c, 1u, &fake_rel, &m);
    /* the RAM top moves DOWN mid-call (the switch-25 legacy force): the model's guard refuses a page it had kept */
    {
        (void)n48_hm_open(&c, N48_HM_ON, 1u);
        Model md { &c, &m, 1u, {}, 1ull << 40 };
        md.pt[0x400000000ull] = 0x800100000ull;
        uint32_t d = 0u;
        const bool r1 = md.read(0x400000000ull, &d, 1u);
        md.ramTop = 0x800000000ull;
        const bool r2 = md.read(0x400000000ull, &d, 1u);
        expect_u("H4 a page kept while the bound allowed it is refused once the bound moves (the guard is asked every access)",
                 r1 && !r2 ? 1u : 0u, 1u);
        (void)n48_hm_close(&c, 1u, &fake_rel, &m);
    }
    if (s.empty()) return;
    const std::string rc = body_from(s, "static uint32_t gfxc_read_core(const GfxcVm &vm, uint64_t va, uint32_t *dst, uint32_t n, uint32_t *sysPages, n48_cg_pagerec *rec) {", "\n}\n");
    expect_u("H4 kext read: the walk, then the guard (and its break), then hm100_mine()",
             ordered(rc, { "if (!gfxc_page(vm, cur, page, isSys)) break;", "if (const uint32_t rr = n48_rt_refuse(page, gRamTopUse)) {",
                           "break;", "if (hm100_mine()) ok = hm100_host_read(page, off, &dst[got], k);" }) ? 1u : 0u, 1u);
    const std::string ws = body_from(s, "static uint32_t gfxc_write_sys(const GfxcVm &vm, uint64_t va, const uint32_t *src, uint32_t n,", "\n}\n");
    expect_u("H4 kext write: the walk, VRAM refused, then the guard, then hm100_mine()",
             ordered(ws, { "if (!gfxc_page(vm, cur, page, isSys)) break;", "if (!isSys) break;",
                           "if (n48_rt_refuse(page, gRamTopUse)) break;", "if (hm100_mine()) ok = hm100_host_write(page, off, &src[put], k);" }) ? 1u : 0u, 1u);
    expect_u("H4 kext: the cached read and write hand n48_hm_get the guard's verdict for `page` again",
             count(s, "n48_hm_get(&gHm, (uintptr_t)current_thread(), page, n48_rt_refuse(page, gRamTopUse), 0u, N48_HM_DIR_IN,") == 1u &&
             count(s, "n48_hm_get(&gHm, (uintptr_t)current_thread(), page, n48_rt_refuse(page, gRamTopUse), 1u, N48_HM_DIR_OUTIN,") == 1u, 1u);
}

static void h5()
{
    uint64_t mism = 0ull, reads = 0ull, remaps = 0ull, badDir = 0ull;
    for (uint32_t mode : { N48_HM_ON, N48_HM_SHADOW }) {
        static n48_hm c {};
        FakeMem m; gM = &m;
        std::mt19937 rng(0x543u + mode);
        for (uint32_t call = 0; call < 400u; call++) {
            TScope sc(&c, &m, mode, 1u);
            Model md { &c, &m, 1u, {}, 1ull << 40 };
            std::map<uint64_t, uint64_t> &pt = md.pt;
            for (uint32_t k = 0; k < 24u; k++) pt[0x400000000ull + k * 4096ull] = 0x20000000ull + (rng() % 64u) * 4096ull;
            for (uint32_t step = 0; step < 200u; step++) {
                const uint64_t va = 0x400000000ull + (rng() % 24u) * 4096ull + (rng() % 1024u) * 4u;
                const uint32_t op = rng() % 10u;
                if (op == 0u) { pt[va & ~0xfffull] = 0x20000000ull + (rng() % 64u) * 4096ull; remaps++; continue; }   /* remap mid-call */
                if (op == 1u) {
                    const uint32_t v = rng(); uint32_t wd = 0u;
                    if (md.write(va, &v, 1u, &wd) && wd != N48_HM_DIR_OUTIN) badDir++;
                    continue;
                }
                uint32_t got = 0u;
                if (!md.read(va, &got, 1u)) continue;
                reads++;
                const uint32_t want = m.page(pt[va & ~0xfffull])[(va & 0xfffu) / 4u];   /* the uncached oracle: the page the walk names NOW */
                if (got != want) mism++;
            }
        }
        expect_u(mode == N48_HM_ON ? "H5 ON: live maps 0 after 400 calls" : "H5 SHADOW: live maps 0 after 400 calls", m.live, 0u);
        if (mode == N48_HM_SHADOW) expect_u("H5 SHADOW: no difference counted on a coherent machine", c.st.differ, 0u);
        if (mode == N48_HM_ON) expect_u("H5 ON: the cache hit (it is exercised)", c.st.hits > 1000u ? 1u : 0u, 1u);
    }
    expect_u("H5 randomized remaps / writes mid-call: every read equals the page the walk names now (0 mismatches)", mism, 0u);
    expect_u("H5   reads and remaps exercised", reads > 50000u && remaps > 10000u ? 1u : 0u, 1u);
    expect_u("H7 every write in H5 went through an OUTIN map", badDir, 0u);
    /* the direct remap: read VA -> A, remap VA -> B, read again */
    static n48_hm c {};
    FakeMem m; gM = &m;
    m.page(0x30000000ull)[0] = 0xAAAAAAAAu; m.page(0x30001000ull)[0] = 0xBBBBBBBBu;
    TScope sc(&c, &m, N48_HM_ON, 1u);
    Model md { &c, &m, 1u, {}, 1ull << 40 };
    md.pt[0x400000000ull] = 0x30000000ull;
    uint32_t a = 0u, b = 0u;
    (void)md.read(0x400000000ull, &a, 1u);
    md.pt[0x400000000ull] = 0x30001000ull;
    (void)md.read(0x400000000ull, &b, 1u);
    expect_u("H5 ON: VA -> A read A; remapped VA -> B reads B (not A's kept map)", a == 0xAAAAAAAAu && b == 0xBBBBBBBBu, 1u);
}

static void h6(const std::string &s)
{
    static n48_hm c {};
    FakeMem m; gM = &m;
    TScope sc(&c, &m, N48_HM_ON, 1u);
    Model md { &c, &m, 1u, {}, 1ull << 40 };
    md.pt[0x400000000ull] = 0x40000000ull;
    md.pt[0x500000000ull] = 0x40000000ull;   /* an alias */
    uint32_t x = 0u;
    (void)md.read(0x400000000ull, &x, 1u);
    (void)md.read(0x500000000ull, &x, 1u);
    expect_u("H6 two VAs aliasing one physical page share ONE entry (1 map, 1 hit)", c.n == 1u && m.made == 1u && c.st.hits == 1u, 1u);
    if (s.empty()) return;
    expect_u("H6 kext: the cached path is keyed by `page`, never `cur`/`va`",
             count(s, "hm100_host_read(page, off,") == 1u && count(s, "hm100_host_write(page, off,") == 1u &&
             count(s, "hm100_host_read(cur") == 0u && count(s, "hm100_host_write(cur") == 0u && count(s, "hm100_host_read(va") == 0u &&
             count(s, "hm100_host_write(va") == 0u, 1u);
}

static void h7(const std::string &s)
{
    static n48_hm c {};
    FakeMem m; gM = &m;
    (void)n48_hm_open(&c, N48_HM_ON, 1u);
    expect_u("H7 an IN insert is refused (counted)", n48_hm_insert(&c, 0x1000ull, N48_HM_DIR_IN, nullptr, nullptr, 0ull) == 0u &&
             c.st.dirrefused == 1u && c.n == 0u, 1u);
    Model md { &c, &m, 1u, {}, 1ull << 40 };
    md.pt[0x400000000ull] = 0x50000000ull;
    uint32_t x = 0u, rdir = 0u, wdir = 0u;
    (void)md.read(0x400000000ull, &x, 1u, &rdir);
    const uint32_t v = 0x12345678u;
    (void)md.write(0x400000000ull, &v, 1u, &wdir);
    expect_u("H7 a page first READ is kept OUTIN, and the later WRITE hits it OUTIN", rdir == N48_HM_DIR_OUTIN && wdir == N48_HM_DIR_OUTIN &&
             c.st.wr_hits == 1u && m.page(0x50000000ull)[0] == 0x12345678u, 1u);
    uint32_t in = 0u;
    for (uint32_t d : m.dirs) if (d != N48_HM_DIR_OUTIN) in++;
    expect_u("H7 every map the owner made was OUTIN", in, 0u);
    (void)n48_hm_close(&c, 1u, &fake_rel, &m);
    if (s.empty()) return;
    const std::string mk = body_from(s, "static uint32_t hm100_mk(void *, uint64_t page, uint32_t dir, void **mdOut, void **mapOut, uint64_t *kva)", "\n}\n");
    expect_u("H7 kext: hm100_mk maps, prepares and completes in the direction it was asked (OUTIN -> kIODirectionOutIn)",
             count(mk, "const IODirection d = dir == N48_HM_DIR_OUTIN ? kIODirectionOutIn : kIODirectionIn;") == 1u &&
             count(mk, "IOMemoryDescriptor::withPhysicalAddress((IOPhysicalAddress)page, 4096, d);") == 1u &&
             count(mk, "md->prepare(d)") == 1u && count(mk, "kIODirectionIn)") == 0u, 1u);
    const std::string rel = body_from(s, "static void hm100_rel(void *, void *mdv, void *mapv, uint32_t dir)", "\n}\n");
    expect_u("H7 kext: hm100_rel completes in the map's own direction",
             count(rel, "const IODirection d = dir == N48_HM_DIR_OUTIN ? kIODirectionOutIn : kIODirectionIn;") == 1u, 1u);
}

static void h8()
{
    static n48_hm c {};
    FakeMem m; gM = &m;
    (void)n48_hm_open(&c, N48_HM_SHADOW, 1u);
    Model md { &c, &m, 1u, {}, 1ull << 40 };
    md.pt[0x400000000ull] = 0x60000000ull;
    m.page(0x60000000ull)[0] = 0x11111111u;
    m.stale[0x60000000ull] = std::vector<uint32_t>(1024, 0xDEADBEEFu);   /* the FIRST (kept) map shows a stale copy */
    uint32_t x = 0u;
    (void)md.read(0x400000000ull, &x, 1u);   /* miss: the stale map is kept (and used once) */
    m.staleOff = true;                        /* later maps show the real page */
    uint32_t y = 0u;
    (void)md.read(0x400000000ull, &y, 1u);
    expect_u("H8 SHADOW: the second read used the FRESH map (the real value)", y, 0x11111111u);
    expect_u("H8 SHADOW: the stale kept map is counted as a difference (read)", c.st.differ == 1u && c.ndis == 1u && c.dis[0].write == 0u &&
             c.dis[0].cached == 0xDEADBEEFu && c.dis[0].fresh == 0x11111111u, 1u);
    const uint32_t v = 0x22222222u; uint32_t wd = 0u;
    (void)md.write(0x400000000ull, &v, 1u, &wd);
    expect_u("H8 SHADOW: a write through the fresh map, and the stale kept map is counted (write)", c.st.differ == 2u && c.dis[1].write == 1u &&
             m.page(0x60000000ull)[0] == 0x22222222u, 1u);
    (void)n48_hm_close(&c, 1u, &fake_rel, &m);
    expect_u("H8 live maps 0", m.live, 0u);
    /* ON never maps on a hit */
    (void)n48_hm_open(&c, N48_HM_ON, 1u);
    Model on { &c, &m, 1u, {}, 1ull << 40 };
    on.pt[0x400000000ull] = 0x61000000ull;
    (void)on.read(0x400000000ull, &x, 1u);
    const uint64_t made = m.made;
    for (uint32_t i = 0; i < 50u; i++) (void)on.read(0x400000000ull + i * 4u, &x, 1u);
    expect_u("H8 ON: 50 hits make no map", m.made, made);
    (void)n48_hm_close(&c, 1u, &fake_rel, &m);
}

static void h9(const std::string &s)
{
    uint32_t mode = N48_HM_OFF;
    expect_u("H9 the setter: 1/2/3 accepted, 0/4/0xFF refused unchanged",
             n48_hm_set(1u, &mode) == 1 && mode == 1u && n48_hm_set(3u, &mode) == 1 && mode == 3u && n48_hm_set(2u, &mode) == 1 && mode == 2u &&
             n48_hm_set(0u, &mode) == 0 && n48_hm_set(4u, &mode) == 0 && n48_hm_set(0xFFu, &mode) == 0 && mode == 2u, 1u);
    expect_u("H9 100 is mid-arm guarded (reads allowed)", n48_cm_cont_switch_refused(100u, 0u, 1u, (uint32_t)N48_CM_SHOT_ARMED) == 1u &&
             n48_cm_cont_switch_refused(100u, 1u, 1u, (uint32_t)N48_CM_SHOT_ARMED) == 0u, 1u);
    if (s.empty()) return;
    expect_u("H9 kext: boot OFF", count(s, "static volatile uint32_t gHm100Mode { N48_HM_OFF };"), 1u);
    expect_u("H9 kext: the verb is gHm100Mode's only writer", count(s, "__atomic_store_n(&gHm100Mode,"), 1u);
    const std::string v = body_from(s, "    } else if ((arg & 0xffull) == 100ull) {", "    } else if ((arg & 0xffull) == 101ull) {");
    expect_u("H9 kext: the verb asks the mid-arm guard with 100",
             count(v, "n48_cm_cont_switch_refused(100u, m == 0u ? 1u : 0u, gXdShot.cont, gXdShot.state)"), 1u);
    expect_u("H9 kext: the scope opens nothing while OFF", count(s, "if (m != N48_HM_OFF) hm100_open(m);"), 1u);
    /* 0.0.542's uncached lines, intact, inside the else branches */
    expect_u("H9 kext: the read's uncached map is 0.0.542's (kIODirectionIn), once",
             count(s, "if (IOMemoryDescriptor *md = IOMemoryDescriptor::withPhysicalAddress((IOPhysicalAddress)page, 4096, kIODirectionIn)) {"), 1u);
    expect_u("H9 kext: the write's uncached map is 0.0.542's (kIODirectionOutIn), once",
             count(s, "if (IOMemoryDescriptor *md = IOMemoryDescriptor::withPhysicalAddress((IOPhysicalAddress)page, 4096,\n"
                      "                                                                             kIODirectionOutIn)) {"), 1u);
    expect_u("H9 kext: the else branches close right after 0.0.542's timing lines",
             count(s, "            if (pfM) pf_hmap(N48_PF_T_HMAP_R, pfM, page);\n            }\n") == 1u &&
             count(s, "        if (pfM) pf_hmap(N48_PF_T_HMAP_W, pfM, page);\n        }\n") == 1u, 1u);
    expect_u("H9 kext: the continuous STOP / START call hm100's report", count(s, "        hm100_arm_stop();") == 1u &&
             count(s, "                  hm100_arm_start();") == 1u, 1u);
}

static void h10()
{
    n48_hm_st st; std::memset(&st, 0xff, sizeof st);
    char b[2048];
    uint32_t ok = 1u;
    const char *hows[] = { " - `gfxneuter 100` read only, unchanged", " - `gfxneuter 100` REFUSED - a continuous arm stands, unchanged",
                           " - `gfxneuter 100` CHANGED BY THIS VERB (counters reset)", " - `gfxneuter 100` REFUSED (unknown M), unchanged",
                           " - CONTINUOUS STOP: no START snapshot, totals" };
    for (uint32_t mode : { N48_HM_ON, N48_HM_OFF, N48_HM_SHADOW })
        for (const char *h : hows) {
            const int n = std::snprintf(b, sizeof b, N48_HM_FMT, N48_HM_ARGS(mode, h, &st));
            if (n < 0 || (uint32_t)n > N48_LOG_CAP_BODY) { ok = 0u; std::printf("  too long (%d): %s\n", n, b); }
        }
    {
        const int n2 = std::snprintf(b, sizeof b, N48_HM_FMT2, N48_HM_ARGS2(&st));
        if (n2 < 0 || (uint32_t)n2 > N48_LOG_CAP_BODY) { ok = 0u; std::printf("  too long (%d): %s\n", n2, b); }
    }
    n48_hm_dis d { ~0ull, 0xffffffffu, 0xffffffffu, 0xffffffffu, 0xffffffffu, 1u, 0u };
    const int n = std::snprintf(b, sizeof b, N48_HM_DIS_FMT, N48_HM_DIS_ARGS(4294967295u, &d));
    if (n < 0 || (uint32_t)n > N48_LOG_CAP_BODY) { ok = 0u; std::printf("  too long (%d): %s\n", n, b); }
    expect_u("H10 every hostmap100 line fits 491 bytes at maximal fields", ok, 1u);
}

int main(int argc, char **argv)
{
    const std::string s = argc > 1 ? slurp(argv[1]) : std::string();
    expect_u("AppleHardwareHook.cpp read (argv[1])", s.empty() ? 0u : 1u, 1u);
    h1();
    h2(s);
    h3(s);
    h4(s);
    h5();
    h6(s);
    h7(s);
    h8();
    h9(s);
    h10();
    std::printf("%s: %d run, %d failed\n", gFail ? "FAIL" : "PASS", gRun, gFail);
    return gFail ? 1 : 0;
}
