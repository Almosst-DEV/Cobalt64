// native_hostimport_test.cpp - build 0.0.612 (milestone #11 step 11c, an internal design note sections 3 and 4): W4 BoImportHost, selector 21 of ABI 1.9.
//   clang++ -std=c++17 -Wall -Wextra -Werror -O1 -fsanitize=address,undefined -fno-sanitize-recover=all \
//       -I src/navi48-bringup/src -I src/navi48-bringup/src/amd src/navi48-bringup/tests/native_hostimport_test.cpp -o /tmp/native_hostimport && /tmp/native_hostimport .
//   (run from the repo root; the argument is the repo root the source pins read from; tests/native_ws_open_plant.sh plants breaks)
// Covers:
//   H1  import_check: every argument rule (4 KiB alignment of the host va and the size, size 0, > 64 MiB per BO, the user half, wrap, flags, the GPU VA), the per-client 4 GiB cap (0.0.621; 2 GiB in 0.0.620, 256 MiB before) and its
//       accounting (separate from the 512 MiB GTT cap), free restoring the room, a stray double free never wrapping the cap open;
//   H2  collect_pages over a fake descriptor: scattered runs, every failure (no segment, short run, unaligned or zero address, an address above the PTE field, page-count mismatch);
//   H3  pt_map_pages into a real 4-level tree (a fake memory): scattered pages resolve to their own physical pages with SYSTEM|SNOOPED (the GTT leaf flags), across PTB boundaries, in
//       both halves; unmap clears; a failed map leaves NO partial mapping; the scattered mapper equals the contiguous pt_map where the pages ARE consecutive;
//   H4  the lifecycle: a session model over the pure functions shows import -> map -> free and the CLOSE of a client with imports freeing every one (cap back to 0), release ordering
//       (unmap and TLB flush before complete(); the HUNG leak never completes or releases), host BOs never scanout / fence / CPU-map targets;
//   H6  review items A / C / D: import_page_verdict (BAR edges, 64-bit BAR above 4 GiB, unaligned and wrapping BARs, DRAM top, unknown = 0, fail closed), the session-sequence ordering model against
//       close / close+open while the range is wired, memory_may_free_after_flush, and the source pins of the wiring;
//   H5  source pins of the REAL code: the ordering inside n1c_bo_import_host, bo_release, host_release and n1c_close; the selector wiring and shape; the refusals; the stack rule (no big local).
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <map>
#include <array>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include "amd/native_hostimport_pure.h"
#include "Navi48NativeABI.h"

using namespace n48native;
using namespace n48native::s1c;

static int gFail = 0, gRun = 0;
static void expect(bool ok, const char *what) { gRun++; if (!ok) { gFail++; std::printf("FAIL: %s\n", what); } }
static void expect_u(const char *what, uint64_t got, uint64_t want) {
    gRun++;
    if (got != want) { gFail++; std::printf("FAIL: %s: got %#llx want %#llx\n", what, (unsigned long long)got, (unsigned long long)want); }
}
static std::string slurp(const std::string &path) { std::ifstream f(path, std::ios::binary); std::stringstream ss; ss << f.rdbuf(); return ss.str(); }
static size_t count_of(const std::string &s, const std::string &needle) { size_t n = 0, p = 0; while ((p = s.find(needle, p)) != std::string::npos) { n++; p += needle.size(); } return n; }
static bool in_order(const std::string &s, std::initializer_list<const char *> parts) {   // 0.0.627
    size_t at = 0;
    for (const char *p : parts) { const size_t f = s.find(p, at); if (f == std::string::npos) return false; at = f + 1; }
    return true;
}
static std::string fn_body(const std::string &src, const std::string &sig) {          // from the signature to its closing brace at column 0
    const size_t a = src.find(sig);
    if (a == std::string::npos) return "";
    const size_t e = src.find("\n}\n", a);
    return e == std::string::npos ? src.substr(a) : src.substr(a, e - a + 3);
}
static size_t at(const std::string &s, const std::string &n, size_t from = 0) { return s.find(n, from); }

constexpr uint64_t HV = 0x0000000100000000ull;   // a page-aligned host VA
constexpr uint64_t MiB = 1ull << 20;

// ---- H1 ------------------------------------------------------------------------------------------------------------------------------------------------
static void h1_check() {
    expect_u("limits: 64 MiB per BO", kImportMaxBo, 64 * MiB); expect_u("limits: 4 GiB per client (0.0.621)", kImportCap, 4096 * MiB);
    expect_u("the ABI macros agree (per BO)", N48N_IMPORT_MAX_BO, kImportMaxBo); expect_u("the ABI macros agree (cap)", N48N_IMPORT_CAP, kImportCap);
    expect(kImportCap != kGttCap && kGttCap == 512 * MiB, "the import cap is separate from the 512 MiB GTT cap");
    { const ImportChk c = import_check(HV, 0x1000, 0, 0, 0);            expect(c.rc == kOk && c.pages == 1 && c.gpuStripped == 0, "one page, no map: OK"); }
    { const ImportChk c = import_check(HV, 64 * MiB, 0, 0, 0);          expect(c.rc == kOk && c.pages == 16384, "exactly 64 MiB: OK"); }
    { const ImportChk c = import_check(HV, 64 * MiB + 0x1000, 0, 0, 0); expect_u("64 MiB + 4 KiB: refused (BadArg)", c.rc, kBadArg); }
    { const ImportChk c = import_check(HV, 0, 0, 0, 0);                 expect_u("size 0: refused", c.rc, kBadArg); }
    { const ImportChk c = import_check(HV + 0x800, 0x1000, 0, 0, 0);   expect_u("host va not 4 KiB aligned: refused", c.rc, kBadArg); }
    { const ImportChk c = import_check(HV, 0x1800, 0, 0, 0);            expect_u("size not a 4 KiB multiple: refused", c.rc, kBadArg); }
    { const ImportChk c = import_check(HV + 0x1, 0x1800, 0, 0, 0);      expect_u("both unaligned: refused", c.rc, kBadArg); }
    { const ImportChk c = import_check(0, 0x1000, 0, 0, 0);             expect_u("host va 0: refused", c.rc, kBadArg); }
    { const ImportChk c = import_check(kImportUserMax - 0x1000, 0x1000, 0, 0, 0); expect_u("the last user page: OK", c.rc, kOk); }
    { const ImportChk c = import_check(kImportUserMax - 0x1000, 0x2000, 0, 0, 0); expect_u("a range that leaves the user half: refused", c.rc, kBadArg); }
    { const ImportChk c = import_check(kImportUserMax, 0x1000, 0, 0, 0);          expect_u("a kernel-half va: refused", c.rc, kBadArg); }
    { const ImportChk c = import_check(0xFFFFFFFFFFFFF000ull, 0x2000, 0, 0, 0);   expect_u("a wrapping range: refused", c.rc, kBadArg); }
    // flags
    { const ImportChk c = import_check(HV, 0x1000, N48N_VM_PAGE_READABLE, 0, 0); expect_u("flags without a GPU VA: refused", c.rc, kBadArg); }
    { const ImportChk c = import_check(HV, 0x1000, 1ull << 33, 0, 0);            expect_u("flags above 32 bits: refused", c.rc, kBadArg); }
    { const ImportChk c = import_check(HV, 0x1000, N48N_VM_PAGE_PRT, 0x100000, 0); expect_u("an unknown / refused flag (PRT): refused", c.rc, kBadArg); }
    { const ImportChk c = import_check(HV, 0x1000, N48N_VM_PAGE_READABLE | (6u << 5), 0x100000, 0); expect_u("an unknown MTYPE: refused", c.rc, kBadArg); }
    // map at import
    { const ImportChk c = import_check(HV, 0x4000, N48N_VM_PAGE_READABLE | N48N_VM_PAGE_WRITEABLE, 0x200000, 0);
      expect(c.rc == kOk && c.gpuStripped == 0x200000 && c.pages == 4, "map at import: OK, the stripped VA is reported"); }
    { const ImportChk c = import_check(HV, 0x4000, N48N_VM_PAGE_READABLE, 0xFFFF800000200000ull, 0); expect(c.rc == kOk && c.gpuStripped == 0x800000200000ull, "a canonical high-half VA is stripped"); }
    { const ImportChk c = import_check(HV, 0x4000, N48N_VM_PAGE_EXECUTABLE, 0x200000, 0);        expect_u("neither R nor W: refused", c.rc, kBadArg); }
    { const ImportChk c = import_check(HV, 0x4000, N48N_VM_PAGE_READABLE, 0x200800, 0);          expect_u("an unaligned GPU VA: refused", c.rc, kBadArg); }
    { const ImportChk c = import_check(HV, 0x4000, N48N_VM_PAGE_READABLE, 0x0000800000000000ull, 0); expect_u("a non-canonical GPU VA: refused", c.rc, kBadArg); }
    { const ImportChk c = import_check(HV, 0x4000, N48N_VM_PAGE_READABLE, 0x8000, 0);            expect_u("a GPU VA below 0x10000: refused", c.rc, kBadArg); }
    { const ImportChk c = import_check(HV, 0x4000, N48N_VM_PAGE_READABLE | N48N_VM_MTYPE_UC, 0x200000, 0); expect_u("MTYPE UC accepted", c.rc, kOk); }
    // the cap (0.0.621: 4 GiB; 2 GiB in 0.0.620, 256 MiB before)
    expect_u("the cap is exactly 4 GiB", kImportCap, 4ull << 30);
    expect_u("64 64-MiB imports fill the cap exactly: the last is admitted", import_check(HV, 64 * MiB, 0, 0, 4032 * MiB).rc, kOk);
    expect_u("an import that brings the total to EXACTLY 4 GiB is admitted", import_check(HV, 0x1000, 0, 0, 4096 * MiB - 0x1000).rc, kOk);
    expect_u("the next page after a full 4 GiB is refused (NoMemory)", import_check(HV, 0x1000, 0, 0, 4096 * MiB).rc, kNoMemory);
    expect_u("4 GiB + 4 KiB (one page over) is refused", import_check(HV, 0x2000, 0, 0, 4096 * MiB - 0x1000).rc, kNoMemory);
    expect_u("64 MiB over by one page is refused", import_check(HV, 64 * MiB, 0, 0, 4032 * MiB + 0x1000).rc, kNoMemory);
    expect_u("the 2 GiB boundary (0.0.620's cap) is now admitted: 2 GiB held + 4 KiB", import_check(HV, 0x1000, 0, 0, 2048 * MiB).rc, kOk);
    expect_u("the 2 GiB boundary: 64 MiB on top of 2 GiB is admitted", import_check(HV, 64 * MiB, 0, 0, 2048 * MiB).rc, kOk);
    expect_u("just under the 2 GiB boundary: 2047 MiB held + 2 MiB is admitted", import_check(HV, 2 * MiB, 0, 0, 2047 * MiB).rc, kOk);
    expect_u("the old 256 MiB boundary is admitted (256 MiB held + 4 KiB)", import_check(HV, 0x1000, 0, 0, 256 * MiB).rc, kOk);
    expect_u("3 GiB held + 64 MiB is admitted", import_check(HV, 64 * MiB, 0, 0, 3072 * MiB).rc, kOk);
    expect_u("the cap is checked AFTER the arguments (a bad argument beats a full cap)", import_check(HV + 1, 0x1000, 0, 0, 4096 * MiB).rc, kBadArg);
    expect_u("a wild running total does not wrap the check", import_check(HV, 0x1000, 0, 0, ~0ull).rc, kNoMemory);
    expect_u("a running total just under 2^64 does not wrap the check", import_check(HV, 0x1000, 0, 0, ~0ull - 0xFFFull).rc, kNoMemory);
    expect(would_exceed(4096 * MiB, 1, kImportCap) && !would_exceed(4096 * MiB - 1, 1, kImportCap) && would_exceed(0, 4096 * MiB + 1, kImportCap) && !would_exceed(0, 4096 * MiB, kImportCap), "would_exceed at the 4 GiB edge, both directions");
    // every quantity that scales with the cap, at its maximum
    {
        const uint64_t pagesAtCap = kImportCap / kPage;
        expect_u("4 GiB of pages is 1048576 (fits a uint32 page count)", pagesAtCap, 1048576ull);
        expect(pagesAtCap <= 0xFFFFFFFFull, "the page count of the whole cap fits uint32");
        expect_u("one BO's page list is still 16384 entries (128 KiB), unchanged", (uint64_t)kImportMaxPages, 16384ull);
        expect_u("all page lists at the cap are 8 MiB of kernel heap", pagesAtCap * sizeof(uint64_t), 8 * MiB);
        expect(kImportCap % kImportMaxBo == 0 && kImportCap / kImportMaxBo == 64, "the cap is 64 whole max-size BOs");
    }
    // accounting: separate from the GTT cap
    {
        uint64_t used = 0, gtt = 0; int n = 0;
        gtt = 500 * MiB;                                                  // GTT nearly full: irrelevant to imports
        while (import_check(HV, 64 * MiB, 0, 0, used).rc == kOk) { used += 64 * MiB; n++; }
        expect_u("exactly sixty-four 64 MiB imports fit with the GTT at 500 MiB", (uint64_t)n, 64);
        expect_u("the running total is the cap", used, kImportCap);
        expect(!would_exceed(gtt, 12 * MiB, kGttCap), "the GTT cap has its own room (nothing here touched it)");
        used = import_after_free(used, 64 * MiB); expect_u("a free returns the room", used, 4032 * MiB);
        expect_u("and one more import fits again", import_check(HV, 64 * MiB, 0, 0, used).rc, kOk);
        used = import_after_free(used, 64 * MiB * 65); expect_u("a stray over-free clamps at 0, never wraps", used, 0);
        expect_u("after which the whole cap is available", import_check(HV, 64 * MiB, 0, 0, used).rc, kOk);
    }
}

// ---- H2 ------------------------------------------------------------------------------------------------------------------------------------------------
struct FakeSeg {   // a list of (physical base, length in bytes) runs laid end to end
    std::vector<std::pair<uint64_t, uint64_t>> runs;
    uint64_t phys(uint64_t off, uint64_t *len) {
        uint64_t base = 0;
        for (auto &r : runs) {
            if (off < base + r.second) { *len = r.second - (off - base); return r.first + (off - base); }
            base += r.second;
        }
        *len = 0; return 0;
    }
};
static void h2_collect() {
    uint64_t out[32];
    {   // scattered: 3 runs of 2, 1, 5 pages
        FakeSeg s; s.runs = { { 0x10000000, 0x2000 }, { 0x30000000, 0x1000 }, { 0x20000000, 0x5000 } };
        std::memset(out, 0, sizeof(out));
        expect_u("collect: scattered runs", collect_pages(s, 8 * kPage, out, 32), kOk);
        const uint64_t want[8] = { 0x10000000, 0x10001000, 0x30000000, 0x20000000, 0x20001000, 0x20002000, 0x20003000, 0x20004000 };
        bool same = true; for (int i = 0; i < 8; i++) same = same && out[i] == want[i];
        expect(same, "collect: every page is the right physical page, in order");
    }
    {   // a run longer than the range: only the range's pages are taken
        FakeSeg s; s.runs = { { 0x40000000, 0x100000 } };
        expect_u("collect: a long run, a short range", collect_pages(s, 3 * kPage, out, 32), kOk);
        expect(out[0] == 0x40000000 && out[1] == 0x40001000 && out[2] == 0x40002000, "collect: the first three pages of the run");
    }
    { FakeSeg s; s.runs = { { 0x10000000, 0x2000 } };                    expect_u("collect: the descriptor ends early (no segment)", collect_pages(s, 3 * kPage, out, 32), kNoMemory); }
    { FakeSeg s; s.runs = { { 0x10000000, 0x800 }, { 0x20000000, 0x10000 } }; expect_u("collect: a run shorter than a page", collect_pages(s, 3 * kPage, out, 32), kNoMemory); }
    { FakeSeg s; s.runs = { { 0x10000800, 0x4000 } };                    expect_u("collect: an unaligned physical address", collect_pages(s, 3 * kPage, out, 32), kNoMemory); }
    { FakeSeg s; s.runs = { { 0, 0x4000 } };                             expect_u("collect: physical address 0", collect_pages(s, 3 * kPage, out, 32), kNoMemory); }
    { FakeSeg s; s.runs = { { 0x0001000000000000ull, 0x4000 } };         expect_u("collect: an address above the PTE field", collect_pages(s, 3 * kPage, out, 32), kBadArg); }
    { FakeSeg s; s.runs = { { 0x0000FFFFFFFFE000ull, 0x2000 } };         expect_u("collect: the last encodable page is fine", collect_pages(s, 2 * kPage, out, 32), kOk); }
    { FakeSeg s; s.runs = { { 0x0000FFFFFFFFF000ull, 0x2000 } };         expect_u("collect: a run that crosses the PTE field's top", collect_pages(s, 2 * kPage, out, 32), kBadArg); }
    { FakeSeg s; s.runs = { { 0x10000000, 0x100000 } };                  expect_u("collect: more pages than the cap", collect_pages(s, 33 * kPage, out, 32), kBadArg); }
    { FakeSeg s; s.runs = { { 0x10000000, 0x100000 } };                  expect_u("collect: size 0", collect_pages(s, 0, out, 32), kBadArg); }
    { FakeSeg s; s.runs = { { 0x10000000, 0x100000 } };                  expect_u("collect: an unaligned size", collect_pages(s, 0x1800, out, 32), kBadArg); }
    { FakeSeg s; for (int i = 0; i < 16384; i++) s.runs.push_back({ 0x100000000ull + (uint64_t)i * 0x3000, 0x1000 });
      static uint64_t big[kImportMaxPages];
      expect_u("collect: a fully scattered 64 MiB range", collect_pages(s, 64 * MiB, big, kImportMaxPages), kOk);
      expect(big[16383] == 0x100000000ull + 16383ull * 0x3000, "collect: the last of 16384 scattered pages"); }
}

// ---- H3 ------------------------------------------------------------------------------------------------------------------------------------------------
struct FakeMem {   // an abstract page-table memory: tables addressed by their pa
    std::map<uint64_t, std::array<uint64_t, 512>> t;
    uint64_t next = 0x100000000ull, pagesLeft = 1000000; uint64_t writes = 0;
    uint64_t rd(uint64_t pa, uint32_t idx) { auto it = t.find(pa); return it == t.end() ? 0 : it->second[idx]; }
    void wr(uint64_t pa, uint32_t idx, uint64_t v) { auto &tb = t[pa]; tb[idx] = v; writes++; }
    uint64_t alloc_page() { if (pagesLeft == 0) return 0; pagesLeft--; const uint64_t pa = next; next += 0x1000; t[pa] = {}; return pa; }
};
static void h3_map() {
    const uint64_t leafSys = leaf_flags(N48N_VM_PAGE_READABLE | N48N_VM_PAGE_WRITEABLE, false, true);
    expect((leafSys & kPteSystem) != 0 && (leafSys & kPteSnooped) != 0 && (leafSys & kPteValid) != 0 && (leafSys & kPteRead) != 0 && (leafSys & kPteWrite) != 0 && (leafSys & kPteExec) == 0 && (leafSys & kPteMtypeUC) == 0, "the leaf flags are the GTT ones: VALID | SYSTEM | SNOOPED | R | W, cached (no UC)");
    expect_u("... exactly", leafSys, kPteValid | kPteSystem | kPteSnooped | kPteRead | kPteWrite);
    for (int half = 0; half < 2; half++) {
        FakeMem m; const uint64_t root = m.alloc_page();
        const uint64_t va = half ? 0xFFFF800000200000ull : 0x0000000000200000ull;   // both halves
        const uint64_t pages = 1100;                                                // crosses two PTB boundaries
        std::vector<uint64_t> pa(pages);
        for (uint64_t i = 0; i < pages; i++) pa[i] = 0x200000000ull + ((i * 7919ull) % 5000ull) * 0x1000ull;   // scattered, not monotone
        expect_u("pt_map_pages: maps", pt_map_pages(root, va, pages, pa.data(), leafSys, m), kOk);
        bool ok = true, flagsOk = true;
        for (uint64_t i = 0; i < pages; i++) {
            uint64_t leaf = 0; const uint64_t got = pt_walk(root, va + i * kPage, m, &leaf);
            if (got != pa[i]) ok = false;
            if ((leaf & ~kPtePaMask & ~kPteIsPte) != leafSys) flagsOk = false;
        }
        expect(ok, half ? "every page of the high-half mapping resolves to its OWN physical page" : "every page of the low-half mapping resolves to its OWN physical page");
        expect(flagsOk, "every leaf carries exactly SYSTEM|SNOOPED|R|W|VALID (+ the P bit)");
        uint64_t leaf = 0; expect_u("the page after the range is unmapped", pt_walk(root, va + pages * kPage, m, &leaf), 0);
        pt_unmap(root, va, pages, m);
        bool gone = true; for (uint64_t i = 0; i < pages; i++) if (pt_walk(root, va + i * kPage, m, nullptr) != 0) gone = false;
        expect(gone, "pt_unmap clears every scattered leaf");
    }
    {   // 0.0.621: the page-table cost of a FULL 4 GiB cap. The per-client page-table pool is kPtCap (32 MiB = 8192 table pages, shared with every other mapping).
        // (a) 4 GiB packed (64 x 64 MiB, back to back): ~2048 leaf tables; (b) the worst realistic spread: 4096 imports of 1 MiB each, every one in its OWN 2 MiB window (the BO table's 4095 handles bind first);
        // (c) the pathological case, every 1 MiB import straddling a 2 MiB boundary and none sharing a table (two leaf tables each, one per 4 MiB of VA): 4095 of them need ~8190 leaf tables and do NOT fit the 8192-page pool (a clean NoMemory, nothing half-mapped).
        const uint64_t poolPages = kPtCap / kPage;
        expect_u("the page-table pool is 8192 table pages", poolPages, 8192ull);
        { FakeMem m; const uint64_t root = m.alloc_page(); std::vector<uint64_t> pa(kImportMaxPages); for (uint64_t i = 0; i < kImportMaxPages; i++) pa[i] = 0x200000000ull + ((i * 7919ull) % 20000ull) * 0x1000ull;
          bool ok = true; for (uint64_t b = 0; b < kImportCap / kImportMaxBo; b++) ok = ok && pt_map_pages(root, 0x10000000ull + b * kImportMaxBo, kImportMaxPages, pa.data(), leafSys, m) == kOk;
          const uint64_t used = 1000000ull - m.pagesLeft;
          expect(ok, "4 GiB packed: all 64 maps succeed"); expect(used >= 2048 && used < 2200, "4 GiB packed needs ~2048 leaf tables plus a few directories"); expect(used < poolPages / 2, "... under half of the client's table pool"); }
        { FakeMem m; const uint64_t root = m.alloc_page(); std::vector<uint64_t> pa(256); for (uint64_t i = 0; i < 256; i++) pa[i] = 0x200000000ull + i * 0x3000ull;
          bool ok = true; for (uint64_t b = 0; b < 4096; b++) ok = ok && pt_map_pages(root, 0x10000000ull + b * 0x200000ull, 256, pa.data(), leafSys, m) == kOk;   // 1 MiB each, one per 2 MiB of VA
          const uint64_t used = 1000000ull - m.pagesLeft;
          expect(ok, "4096 x 1 MiB spread over 8 GiB of VA: all maps succeed"); expect(used >= 4096 && used < poolPages, "... needs ~4096 leaf tables plus directories: inside the 8192-page pool"); expect(used < 4200, "... about 4096 + a handful of directory pages"); }
        { FakeMem m; m.pagesLeft = poolPages; const uint64_t root = m.alloc_page(); std::vector<uint64_t> pa(256); for (uint64_t i = 0; i < 256; i++) pa[i] = 0x200000000ull + i * 0x3000ull;
          uint64_t okN = 0, firstFail = 0; for (uint64_t b = 0; b < 4095; b++) { const uint32_t r = pt_map_pages(root, 0x10000000ull + b * 0x400000ull + 0x180000ull, 256, pa.data(), leafSys, m); if (r == kOk) okN++; else { firstFail = b; expect_u("the straddling worst case fails CLEANLY with NoMemory", r, kNoMemory); break; } }
          expect(okN > 4000 && okN < 4095 && firstFail == okN, "4095 straddling 1 MiB imports exhaust the 8192-page pool a few short of 4095 (documented limit: such a layout is pathological, RADV packs)"); }
    }
    {   // a sub-range: GemVa MAP with offset_in_bo maps the slice of the page list
        FakeMem m; const uint64_t root = m.alloc_page();
        std::vector<uint64_t> pa = { 0x300000000ull, 0x310000000ull, 0x320000000ull, 0x330000000ull, 0x340000000ull };
        expect_u("a slice [1..4) maps", pt_map_pages(root, 0x400000, 3, pa.data() + 1, leafSys, m), kOk);
        expect(pt_walk(root, 0x400000, m, nullptr) == pa[1] && pt_walk(root, 0x402000, m, nullptr) == pa[3], "the slice's pages, not the BO's first pages");
    }
    {   // it equals the contiguous mapper where the pages ARE consecutive
        FakeMem a, b; const uint64_t ra = a.alloc_page(), rb = b.alloc_page();
        std::vector<uint64_t> pa; for (uint64_t i = 0; i < 700; i++) pa.push_back(0x500000000ull + i * kPage);
        expect_u("contiguous mapper", pt_map(ra, 0x600000, 700, 0x500000000ull, leafSys, a), kOk);
        expect_u("scattered mapper", pt_map_pages(rb, 0x600000, 700, pa.data(), leafSys, b), kOk);
        bool same = true; for (uint64_t i = 0; i < 700; i++) { uint64_t la = 0, lb = 0; same = same && pt_walk(ra, 0x600000 + i * kPage, a, &la) == pt_walk(rb, 0x600000 + i * kPage, b, &lb) && la == lb; }
        expect(same, "same leaves as pt_map for consecutive pages");
    }
    {   // failure leaves no partial mapping
        FakeMem m; const uint64_t root = m.alloc_page(); const uint64_t w0 = m.writes;
        std::vector<uint64_t> bad = { 0x300000000ull, 0x310000000ull, 0x0001000000000000ull, 0x330000000ull };
        expect_u("a page above the PTE field: BadArg", pt_map_pages(root, 0x400000, 4, bad.data(), leafSys, m), kBadArg);
        expect_u("... and nothing was written (no table, no leaf)", m.writes, w0);
        std::vector<uint64_t> unal = { 0x300000000ull, 0x310000800ull };
        expect_u("an unaligned page: BadArg", pt_map_pages(root, 0x400000, 2, unal.data(), leafSys, m), kBadArg);
        expect_u("... and nothing was written", m.writes, w0);
        expect_u("no pages: BadArg", pt_map_pages(root, 0x400000, 0, bad.data(), leafSys, m), kBadArg);
        expect_u("no list: BadArg", pt_map_pages(root, 0x400000, 2, nullptr, leafSys, m), kBadArg);
        // out of page-table space: refused before ANY leaf is written
        FakeMem full; const uint64_t r2 = full.alloc_page(); full.pagesLeft = 2;   // the tree needs 3 tables below the root
        std::vector<uint64_t> ok = { 0x300000000ull, 0x310000000ull };
        expect_u("no table space: NoMemory", pt_map_pages(r2, 0x400000, 2, ok.data(), leafSys, full), kNoMemory);
        bool leafSeen = false; for (auto &kv : full.t) for (uint64_t e : kv.second) if ((e & kPteIsPte) != 0) leafSeen = true;
        expect(!leafSeen, "... and no leaf entry exists (no partial mapping)");
    }
}

// ---- H4 ------------------------------------------------------------------------------------------------------------------------------------------------
// A session model over the REAL pure functions. Events are recorded so the ORDER the kernel code must follow is checked; the source pins (H5) tie the order to native_s1c.cpp.
namespace {
struct Ev { std::vector<std::string> log; };
struct Bo { uint64_t size; uint32_t pages; bool mapped; bool alive; };
struct Sess {
    std::vector<Bo> bo; uint64_t imported = 0; FakeMem mem; uint64_t root = 0; Ev ev; bool hung = false; int completes = 0, releases = 0;
    Sess() { root = mem.alloc_page(); }
    uint32_t import_(uint64_t size, bool map, uint64_t gpuVa) {
        const ImportChk c = import_check(HV, size, map ? (uint64_t)(N48N_VM_PAGE_READABLE | N48N_VM_PAGE_WRITEABLE) : 0ull, map ? gpuVa : 0ull, imported);
        if (c.rc != kOk) return c.rc;
        std::vector<uint64_t> pa(c.pages); for (uint64_t i = 0; i < c.pages; i++) pa[i] = 0x800000000ull + (bo.size() * 100000ull + i) * kPage;
        if (map && pt_map_pages(root, c.gpuStripped, c.pages, pa.data(), leaf_flags(N48N_VM_PAGE_READABLE | N48N_VM_PAGE_WRITEABLE, false, true), mem) != kOk) return kNoMemory;
        bo.push_back({ size, (uint32_t)c.pages, map, true }); imported += size; ev.log.push_back("import");
        return kOk;
    }
    // bo_release's host part in each mode (the order the code follows): Normal = unmap, flush, THEN complete + release; Closing = (the caller parked CONTEXT8 + flushed) THEN complete + release; Leak = nothing.
    void release(size_t i, uint32_t mode, uint64_t gpuVa) {
        Bo &b = bo[i];
        if (mode == kHostRelNormal && b.mapped) { pt_unmap(root, gpuVa, b.pages, mem); ev.log.push_back("unmap"); ev.log.push_back("flush"); }
        if (host_may_release(mode)) { ev.log.push_back("complete"); ev.log.push_back("release"); completes++; releases++; }
        imported = import_after_free(imported, b.size); b.alive = false;
    }
};
}
static void h4_lifecycle() {
    {   // import, map, free (BoFree, Normal)
        Sess s; expect_u("import 1 MiB, map at import", s.import_(1 * MiB, true, 0x400000), kOk);
        expect_u("the total counts it", s.imported, 1 * MiB);
        expect(pt_walk(s.root, 0x400000, s.mem, nullptr) != 0, "it is mapped");
        s.release(0, kHostRelNormal, 0x400000);
        expect_u("BoFree: the total is back to 0", s.imported, 0);
        expect(pt_walk(s.root, 0x400000, s.mem, nullptr) == 0, "BoFree: the mapping is gone");
        const std::vector<std::string> want = { "import", "unmap", "flush", "complete", "release" };
        expect(s.ev.log == want, "BoFree order: unmap, TLB flush, THEN complete, then release");
    }
    {   // CLOSE of a client holding several imports frees every one (Closing)
        Sess s; uint64_t va = 0x1000000;
        for (int i = 0; i < 6; i++) { expect_u("import", s.import_((uint64_t)(i + 1) * MiB, i % 2 == 0, va), kOk); va += 0x1000000; }
        expect_u("six imports held", s.bo.size(), 6); expect_u("the total", s.imported, (1 + 2 + 3 + 4 + 5 + 6) * MiB);
        s.ev.log.clear();
        s.ev.log.push_back("park"); s.ev.log.push_back("flush");            // n1c_close: program_root(gPark.pa) parks CONTEXT8 and flushes the TLB first
        for (size_t i = 0; i < s.bo.size(); i++) s.release(i, kHostRelClosing, 0);
        expect_u("close: every import completed", (uint64_t)s.completes, 6); expect_u("close: every import released", (uint64_t)s.releases, 6);
        expect_u("close: the running total is 0", s.imported, 0);
        bool allDead = true; for (auto &b : s.bo) if (b.alive) allDead = false;
        expect(allDead, "close: no BO survives");
        expect(s.ev.log[0] == "park" && s.ev.log[1] == "flush" && s.ev.log[2] == "complete", "close: CONTEXT8 is parked and the TLB flushed BEFORE the first complete()");
        size_t firstComplete = 0; for (size_t i = 0; i < s.ev.log.size(); i++) if (s.ev.log[i] == "complete") { firstComplete = i; break; }
        expect(firstComplete == 2, "close: nothing is completed before the park");
    }
    {   // HUNG: leak, never complete or release
        Sess s; expect_u("import", s.import_(4 * MiB, true, 0x400000), kOk); s.ev.log.clear();
        s.release(0, kHostRelLeak, 0x400000);
        expect_u("HUNG: complete() is never called", (uint64_t)s.completes, 0); expect_u("HUNG: release() is never called", (uint64_t)s.releases, 0);
        expect(s.ev.log.empty(), "HUNG: nothing at all touched the pages (no unmap, no flush, no complete, no release)");
        expect(pt_walk(s.root, 0x400000, s.mem, nullptr) != 0, "HUNG: the PTEs stay (the GPU might still read the pages)");
    }
    expect(host_may_release(kHostRelNormal) && host_may_release(kHostRelClosing) && !host_may_release(kHostRelLeak), "host_may_release: Normal and Closing yes, the HUNG leak never");
    expect(!host_may_release(3) && !host_may_release(0xFFFFFFFFu), "host_may_release: an unknown mode never releases");
    {   // the cap under churn: import / free many times, the total never drifts
        Sess s; for (int round = 0; round < 10; round++) { for (int i = 0; i < 64; i++) expect_u("import in a round", s.import_(64 * MiB, false, 0), kOk); expect_u("a 65th is refused (4 GiB held)", s.import_(0x1000, false, 0), kNoMemory);
            for (size_t i = s.bo.size() - 64; i < s.bo.size(); i++) s.release(i, kHostRelNormal, 0); expect_u("all 64 freed", s.imported, 0); }
    }
}

// ---- H5 ------------------------------------------------------------------------------------------------------------------------------------------------
static void h5_pins(const std::string &root) {
    const std::string K = root + "/src/navi48-bringup/";
    const std::string eng = slurp(K + "src/amd/native_s1c.cpp"), engh = slurp(K + "src/amd/native_s1c.h"), cli = slurp(K + "src/Navi48NativeClient.cpp"), abi = slurp(K + "src/Navi48NativeABI.h");
    const std::string pure = slurp(K + "src/amd/native_hostimport_pure.h"), plist = slurp(K + "Info.plist");
    expect(!eng.empty() && !engh.empty() && !cli.empty() && !abi.empty() && !pure.empty(), "the sources are readable from the root given");
    // the selector: number, shape, task
    expect_u("BoImportHost is the next free selector, 21", N48N_SEL_BO_IMPORT_HOST, 21); expect_u("ABI minor 12", N48N_ABI_MINOR, 12); expect_u("selector count 1.9", N48N_SEL_COUNT_1_9, 22);
    expect_u("the ABI major is unchanged", N48N_ABI_VERSION, 1);
    { const size_t c = cli.find("case N48N_SEL_BO_IMPORT_HOST:\n\t\tif (!shape(4, 4, 0, 0)) return kIOReturnBadArgument;\n"), r = cli.find("\t\treturn amdgpu::n1c_bo_import_host(r, task, si[0], si[1], si[2], si[3], so);", c == std::string::npos ? 0 : c);
      expect(c != std::string::npos && r != std::string::npos && cli.substr(c, r - c).find("import_caller_ok(current_task(), task)") != std::string::npos,
           "the client dispatches selector 21: exact shape (4 in, 4 out, no structs), then the owning-task check, then the import with the task the client was opened with"); }
    expect(engh.find("IOReturn n1c_bo_import_host(const N1cRef &ref, task_t task, uint64_t hostVa, uint64_t size, uint64_t flags, uint64_t gpuVa, uint64_t out[4]);") != std::string::npos, "the engine declares n1c_bo_import_host");
    expect(abi.find("N48N_SEL_BO_IMPORT_HOST = 21,") != std::string::npos && abi.find("N48N_SEL_COUNT_1_9     = 22") != std::string::npos, "the ABI header names selector 21 and the 1.9 count");
    // n1c_bo_import_host: order
    const std::string imp = fn_body(eng, "IOReturn n1c_bo_import_host(");
    expect(!imp.empty(), "n1c_bo_import_host is found");
    const size_t pHello = at(imp, "if (!sess_hello(ref)) return kIOReturnNotReady;"), pPre = at(imp, "import_check("), pWith = at(imp, "IOMemoryDescriptor::withAddressRange("), pPrep = at(imp, "md->prepare()"),
                 pMal = at(imp, "IOMalloc("), pCol = at(imp, "collect_pages(seg, size, pages, kImportMaxPages)"), pLock = at(imp, "Session *s = sess_lock(ref, false);"), pGate = at(imp, "if (gate != kOk) { rc = (IOReturn)gate; break; }"),
                 pAuth = at(imp, "import_check(hostVa, size, flags, gpuVa, s->importedBytes)"), pMap = at(imp, "pt_map_pages("), pRec = at(imp, "s->bo[h] = b;"), pUnl = at(imp, "IOLockUnlock(gCliLock);");
    expect(pHello != std::string::npos && pPre != std::string::npos && pWith != std::string::npos && pPrep != std::string::npos && pLock != std::string::npos && pGate != std::string::npos && pAuth != std::string::npos && pMap != std::string::npos && pRec != std::string::npos && pUnl != std::string::npos, "every step of the import is present");
    expect(pHello < pPre && pPre < pWith && pWith < pPrep && pPrep < pMal && pMal < pCol && pCol < pLock, "order: Hello, argument check, descriptor, prepare(), page list, page list check - all BEFORE the client lock");
    expect(pLock < pGate && pGate < pAuth && pAuth < pMap && pMap < pRec && pRec < pUnl, "order under the lock: HUNG gate, authoritative cap check, map, record");
    expect(at(imp, "const uint32_t gate = (s != nullptr && s->hello) ? gpu_gate(s) : kOk;") > pLock && at(imp, "const uint32_t gate = (s != nullptr && s->hello) ? gpu_gate(s) : kOk;") < at(imp, "IOLockLock(gCliLock);") && at(imp, "IOLockLock(gCliLock);") < pGate,
           "0.0.627: the gate is evaluated under the SESSION lock only (a stall poll may run a recovery: never under gCliLock), then gCliLock, then it is acted on");
    expect(imp.find("IOMemoryDescriptor::withAddressRange((mach_vm_address_t)hostVa, (mach_vm_size_t)size, kIODirectionInOut, task)") != std::string::npos, "the descriptor is of the CALLER's task, in/out");
    expect(imp.find("kIOMemoryMapperNone") == std::string::npos && eng.find("md->getPhysicalSegment((IOByteCount)off, &seg, kIOMemoryMapperNone)") != std::string::npos, "physical segments are read as CPU physical (kIOMemoryMapperNone), in DescSeg");
    expect(imp.find("leaf_flags((uint32_t)flags, false, true)") != std::string::npos, "the import's leaves are the SYSTEM (snooped) GTT flags");
    { const size_t t = at(imp, "if (!taken) {"); expect(t != std::string::npos && imp.substr(t).find("md->complete(); md->release();") != std::string::npos && imp.substr(t).find("IOFree(pages") != std::string::npos, "a failed import completes and releases the never-mapped descriptor and frees the page list"); }
    expect_u("prepare() is called exactly once, before the client lock", count_of(imp, "prepare()"), 1);
    expect_u("the import is counted exactly once", count_of(imp, "s->importedBytes += size;"), 1);
    expect(imp.find("host_release(") == std::string::npos, "the import path never uses the BO release (nothing is recorded on failure)");
    expect(at(imp, "taken = true;") > pRec && at(imp, "taken = true;") < pUnl, "ownership passes to the BO record exactly at the record");
    expect(imp.find("uint64_t pages[") == std::string::npos && imp.find("uint64_t page[") == std::string::npos, "stack: the page list is heap (IOMalloc), never a local array");
    expect_u("the page list is one IOMalloc and is freed on the failure paths", count_of(imp, "IOMalloc("), 1);
    expect(count_of(imp, "IOFree(pages") >= 2, "the page list is freed on the failure paths");
    expect(imp.find("N48N_PLACED_HOST_IMPORT") != std::string::npos && imp.find("va_canonicalize(c.gpuStripped)") != std::string::npos, "the outputs: handle, size, canonical GPU VA, placed bit");
    // release: bo_release
    const std::string rel = fn_body(eng, "static void bo_release(Session *s, uint32_t h, RelMode mode) {");
    expect(!rel.empty(), "bo_release is found");
    {
        const size_t nrm = at(rel, "} else if (mode == kRelNormal) {"), lk = at(rel, "} else {\n        // Leak:");
        expect(nrm != std::string::npos && lk != std::string::npos, "bo_release has Closing / Normal / Leak branches");
        const std::string normal = rel.substr(nrm, lk - nrm), closing = rel.substr(0, nrm), leak = rel.substr(lk);
        expect(at(normal, "pt_unmap(s->rootPa") < at(normal, "flush_vmid(s->vmid)") && at(normal, "flush_vmid(s->vmid)") < at(normal, "host_release(b)"), "BoFree (Normal): unmap the PTEs, flush the TLB, THEN complete + release");
        expect(normal.find("else if (b.kind == kBoHost && host_may_release(kHostRelNormal) && memOk) { host_release(b); sys_release(b); }") != std::string::npos, "Normal releases through host_may_release (not negated) and only when the flushes acknowledged (0.0.612 review item D)");
        expect(closing.find("else if (b.kind == kBoHost && host_may_release(kHostRelClosing)) { host_release(b); sys_release(b); }") != std::string::npos, "Closing releases through host_may_release (not negated)");
        expect(leak.find("host_release") == std::string::npos && leak.find("complete") == std::string::npos && leak.find("release()") == std::string::npos, "the HUNG leak branch never completes or releases");
        expect(rel.find("else if (b.kind == kBoHost) s->importedBytes = import_after_free(s->importedBytes, b.size);") != std::string::npos, "a freed (or leaked) host BO leaves the cap accounting");
        expect(rel.find("if (b.pinLeak != 0u) mode = kRelLeak;") != std::string::npos, "unchanged: an unverified console restore turns any release into a leak");
    }
    const std::string hr = fn_body(eng, "static void host_release(Bo &b) {");
    expect(!hr.empty() && at(hr, "b.hmd->complete();") < at(hr, "b.hmd->release();") && at(hr, "b.hmd->release();") < at(hr, "IOFree(b.hpages"), "host_release: complete() before release(), the page list last");
    // close
    const std::string cl = fn_body(eng, "void n1c_close(const N1cRef &ref, const char *how) {");
    expect(!cl.empty(), "n1c_close is found");
    expect(at(cl, "program_root(s->vmid, gPark.pa)") < at(cl, "bo_release(s, h, kRelClosing)"), "close: the session's context is parked (and the TLB flushed) BEFORE any BO is released (unmap before complete)");
    expect(at(cl, "bool leak = hung_now();") < at(cl, "program_root(s->vmid, gPark.pa)") && cl.find("if (!leak) {\n        for (uint32_t h = 1; h < N48N_MAX_BOS; h++) if (s->boUsed[h]) bo_release(s, h, kRelClosing);") != std::string::npos, "close: the release loop runs only when not leaking (HUNG)");
    expect(cl.find("LEAKED (HUNG): pages stay wired, never completed or released") != std::string::npos, "close under HUNG says host imports are leaked");
    expect_u("close releases BOs in exactly one place", count_of(cl, "bo_release(s, h, kRelClosing)"), 1);
    // free
    const std::string fr = fn_body(eng, "IOReturn n1c_bo_free(const N1cRef &ref, uint64_t handle) {");
    expect(fr.find("RelMode m = kRelNormal;") != std::string::npos && fr.find("if (hung_now()) m = kRelLeak;") != std::string::npos && fr.find("else if (!own_wait(s, &noBlame)) { m = kRelLeak; rc = kIOReturnTimeout; }") != std::string::npos && in_order(fr, { "own_wait(s, &noBlame)", "IOLockLock(gCliLock);", "bo_release(s, h, m);", "IOLockUnlock(gCliLock);" }),
           "BoFree: HUNG and a timed-out own wait leak; otherwise Normal (after THIS session's work retired, 0.0.627); the release runs under gCliLock AFTER the wait");
    // the other consumers of a BO refuse a host BO
    expect(fn_body(eng, "static uint32_t resolve_fence(").find("if (b.kind == kBoHost) return kNotPermitted;") != std::string::npos, "a host BO is never a user-fence target");
    expect(fn_body(eng, "IOReturn n1c_scan_register(").find("if (b.kind == kBoHost) { rc = kIOReturnNotPermitted; break; }") != std::string::npos, "a host BO is never a scanout slot");
    expect(fn_body(eng, "IOReturn n1c_memory_for_handle(").find("if (b.kind == kBoHi || b.kind == kBoHost) rc = kIOReturnNotPermitted;") != std::string::npos, "a host BO is never CPU-mapped by clientMemoryForType (it would map bogus VRAM)");
    // GemVa
    const std::string gv = fn_body(eng, "IOReturn n1c_gem_va(");
    expect(gv.find("host ? pt_map_pages(s->rootPa, q.stripped, in->map_size / kPage, b.hpages + in->offset_in_bo / kPage, leaf_flags(in->flags, b.uc, sys), mem)") != std::string::npos && gv.find(": pt_map(s->rootPa, q.stripped, in->map_size / kPage, basePa, leaf_flags(in->flags, b.uc, sys), mem);") != std::string::npos,
           "GemVa MAP of a host BO maps the slice of its scattered page list; every other BO still goes through pt_map unchanged");
    expect(gv.find("const bool sys = b.kind == kBoGtt || host;") != std::string::npos, "a host BO is SYSTEM|SNOOPED like a GTT BO");
    // the Bo record and the session
    expect(eng.find("kBoHost = 4") != std::string::npos && eng.find("IOMemoryDescriptor *hmd;") != std::string::npos && eng.find("uint64_t *hpages;") != std::string::npos && eng.find("uint64_t  importedBytes;") != std::string::npos, "the BO record and the session carry the import state");
    expect(eng.find("static_assert(kRelNormal == kHostRelNormal && kRelLeak == kHostRelLeak && kRelClosing == kHostRelClosing") != std::string::npos, "the pure release modes are the kernel's RelMode");
    expect(pure.find("constexpr uint64_t kImportMaxBo   = 64ull << 20;") != std::string::npos && pure.find("constexpr uint64_t kImportCap     = 4096ull << 20;") != std::string::npos, "the limits in the pure header");
    expect(plist.find("<string>0.0.664</string>") != std::string::npos, "Info.plist is 0.0.664");
    {   // 0.0.620: the cap refusal is logged (before and under the lock) with the cap read from the constant, and nothing else in the import path hard-codes 256 MiB
        expect(imp.find("if (pre.rc == kNoMemory) N1C_LOG(\"import refused: per-client cap of %llu MiB reached (") != std::string::npos && imp.find("reached (%llu MiB held, %llu KiB asked)\", (unsigned long long)(kImportCap >> 20), ") != std::string::npos, "the advisory cap refusal is logged with kImportCap");
        expect(imp.find("if (c.rc == kNoMemory) N1C_LOG(\"import refused: per-client cap of %llu MiB reached under the lock (") != std::string::npos && imp.find("reached under the lock (%llu MiB held, %llu KiB asked)\", (unsigned long long)(kImportCap >> 20), ") != std::string::npos, "the authoritative cap refusal is logged with kImportCap");
        expect(imp.find("268435456") == std::string::npos && imp.find("256ull") == std::string::npos && imp.find("<< 20) ==") == std::string::npos, "the import path hard-codes no 256 MiB");
        expect(abi.find("#define N48N_IMPORT_CAP          (4096ull << 20)") != std::string::npos && abi.find("(256ull << 20)") == std::string::npos && abi.find("(2048ull << 20)") == std::string::npos, "the ABI header carries the 4 GiB cap and no 256 MiB or 2 GiB cap");
        expect(abi.find("#define N48N_MAX_BOS       4096u") != std::string::npos && pure.find("kImportMaxPages = (uint32_t)(kImportMaxBo / kPage)") != std::string::npos, "the handle table and the per-BO page list are unchanged (4 GiB at ~1 MB average needs ~4000 of the 4095 handles: the BO table binds first, below ~1 MB average; raising it is NOT contained, see the 0.0.621 report)");
    }
    // no hardware register is written by the import (only the PTEs of the client's own tree through the existing PtMem, and the existing TLB flush)
    expect(imp.find("WREG32") == std::string::npos && imp.find("WBAR0") == std::string::npos && imp.find("WDOORBELL") == std::string::npos && imp.find("ring_emit") == std::string::npos, "no register, doorbell or ring write in the import");
}

// ---- H6 (review items A, C, D) ---------------------------------------------------------------------------------------------------------------------------------
static void h6_device_pages() {
    // this GPU: BAR0 = 16 GiB 64-bit prefetchable above 4 GiB (the FULL size, not the 256 MiB window the kext maps), BAR2 = 2 MiB doorbells, BAR5 = 1 MiB registers, an expansion ROM
    const PciBar bars[] = { { 0x4000000000ull, 16ull << 30 }, { 0xE0000000ull, 2 * MiB }, { 0xFCA00000ull, 1 * MiB }, { 0xFCB00000ull, 128ull << 10 }, { 0, 0 } };
    const uint32_t nb = 5;
    const uint64_t top = 0x880000000ull;   // 34 GiB + 512 MiB
    auto ok = [&](uint64_t pa, uint64_t dt) { return import_page_allowed(pa, bars, nb, dt); };
    // 64-bit BAR above 4 GiB: first page, a page deep inside the part the kext does not map (base + 1 GiB), the LAST page, one past the end, one below the start
    expect(!ok(0x4000000000ull, 0), "BAR0 (64-bit, above 4 GiB): its first page is refused");
    expect(!ok(0x4000000000ull + (1ull << 30), 0), "BAR0: a page 1 GiB in (beyond the 256 MiB mapped window) is refused: the FULL size counts");
    expect(!ok(0x4000000000ull + (16ull << 30) - 0x1000, 0), "BAR0: the page holding the LAST byte is refused");
    expect(ok(0x4000000000ull + (16ull << 30), 0), "BAR0: the page at BAR + size is allowed (BAR check only)");
    expect(ok(0x4000000000ull - 0x1000, 0), "BAR0: the page just below the base is allowed");
    // a 32-bit BAR
    expect(!ok(0xE0000000ull, 0) && !ok(0xE01FF000ull, 0), "BAR2: first page and the page with the last byte refused");
    expect(ok(0xE0200000ull, 0) && ok(0xDFFFF000ull, 0), "BAR2: BAR + size and the page below are allowed");
    expect(!ok(0xFCA00000ull, 0) && !ok(0xFCAFF000ull, 0) && ok(0xFCB00000ull - 0x100000 - 0x1000, 0), "BAR5 edges");
    expect(!ok(0xFCB00000ull, 0) && !ok(0xFCB1F000ull, 0) && ok(0xFCB20000ull, 0), "the expansion ROM is a BAR too (128 KiB)");
    // a size-0 (absent) entry is skipped, and does not refuse address 0 pages
    { const PciBar z[] = { { 0, 0 }, { 0xE0000000ull, 0x1000 } }; expect(import_page_allowed(0x1000, z, 2, 0), "an absent (size 0) BAR refuses nothing"); expect(!import_page_allowed(0xE0000000ull, z, 2, 0), "the present one still refuses"); }
    // an unaligned BAR base: every 4 KiB page it touches is refused
    { const PciBar u[] = { { 0xE0000800ull, 0x1000 } }; expect(!import_page_allowed(0xE0000000ull, u, 1, 0) && !import_page_allowed(0xE0001000ull, u, 1, 0) && import_page_allowed(0xE0002000ull, u, 1, 0), "an unaligned BAR refuses both pages it straddles"); }
    // a BAR that wraps the address space reads as reaching the top (refuses more)
    { const PciBar w[] = { { 0xFFFFFFFFFFFFF000ull, 0x2000 } }; expect(!import_page_allowed(0xFFFFFFFFFFFFF000ull, w, 1, 0) && import_page_allowed(0x1000, w, 1, 0), "a wrapping BAR is read to the top of the address space"); }
    // DRAM top: known
    expect(ok(0x87FFFF000ull, top), "dramTop: the last DRAM page is allowed");
    expect(!ok(0x880000000ull, top), "dramTop: the page AT the top is refused");
    expect(!ok(0x900000000ull, top) && !ok(0x7FFFFFFFF000ull, top), "dramTop: pages above the top are refused");
    expect(ok(0x1000, top) && ok(0x100000000ull, top), "dramTop: ordinary low RAM is allowed");
    expect(!ok(0xE0000000ull, top), "dramTop: a BAR page is still refused when the top is known");
    { const PciBar hi[] = { { 0xE0000000ull, 0x1000 } }; expect(import_page_allowed(0x87FFFF000ull, hi, 1, top) && !import_page_allowed(0x880000000ull, hi, 1, top), "dramTop is judged on the whole page"); }
    // dramTop 0 = unknown: only the BAR test; a page far above any DRAM (and outside every BAR) passes it, exactly as documented
    expect(ok(0x0000100000000000ull, 0), "dramTop 0 (unknown): a page outside every BAR is allowed");
    expect(!ok(0x0000100000000000ull, top), "... and the same page is refused once the top is known");
    // the verdict names the reason; the BAR test wins over the DRAM test
    expect_u("verdict: in a BAR", import_page_verdict(0xE0000000ull, bars, nb, top), kPageInBar);
    expect_u("verdict: above DRAM", import_page_verdict(0x900000000ull, bars, nb, top), kPageAboveDram);
    expect_u("verdict: ok", import_page_verdict(0x1000, bars, nb, top), kPageOk);
    expect_u("verdict: a BAR page that is also above the top is reported as a BAR page", import_page_verdict(0x4000000000ull, bars, nb, top), kPageInBar);
    // FAIL CLOSED: nothing latched
    expect(!import_page_allowed(0x1000, bars, 0, top) && !import_page_allowed(0x1000, nullptr, 3, top) && !import_page_allowed(0x1000, nullptr, 0, 0), "no BAR list latched: every page is refused");
    expect_u("verdict: no bars", import_page_verdict(0x1000, nullptr, 0, 0), kPageNoBars);
    // an address that wraps
    expect(!ok(0xFFFFFFFFFFFFF000ull, 0) && !ok(0xFFFFFFFFFFFFFFF0ull, 0), "a page whose end wraps the address space is refused");
    // the list: the first refused index, n when all pass
    { const uint64_t l[] = { 0x1000, 0x2000, 0xE0000000ull, 0x3000 }; expect_u("first_refused: the BAR page at index 2", import_first_refused(l, 4, bars, nb, 0), 2); }
    { const uint64_t l[] = { 0x1000, 0x2000, 0x3000 }; expect_u("first_refused: all fine -> n", import_first_refused(l, 3, bars, nb, top), 3); }
    { const uint64_t l[] = { 0x1000, 0x2000, 0x3000 }; expect_u("first_refused: nothing latched -> index 0", import_first_refused(l, 3, nullptr, 0, top), 0); }
    { const uint64_t l[] = { 0x1000, 0x2000, 0x900000000ull }; expect_u("first_refused: the LAST page above DRAM is found", import_first_refused(l, 3, bars, nb, top), 2); }
    expect_u("first_refused: an empty list -> 0", import_first_refused(nullptr, 0, bars, nb, top), 0);
    // through the REAL collect_pages: a descriptor whose middle page is device memory is refused whole
    {
        struct Seg { const uint64_t *pa; uint64_t phys(uint64_t off, uint64_t *len) { *len = kPage; return pa[off / kPage]; } };
        const uint64_t pa[] = { 0x10000000ull, 0xFCA00000ull, 0x10002000ull };
        Seg sg { pa }; uint64_t out[3] = {};
        expect_u("collect_pages hands the list over", collect_pages(sg, 3 * kPage, out, 16), kOk);
        expect_u("... and the device page (index 1) is the first refused", import_first_refused(out, 3, bars, nb, top), 1);
    }
    // C: the session sequence
    expect(session_unchanged(5, 5) && session_unchanged(1, 1), "C: the same session: unchanged");
    expect(!session_unchanged(5, 6) && !session_unchanged(5, 0) && !session_unchanged(6, 5), "C: another session or none: changed");
    expect(!session_unchanged(0, 0), "C: no session at entry (0) is never 'unchanged' (0 == 0 must not pass)");
    // D
    expect(memory_may_free_after_flush(0) && !memory_may_free_after_flush(kTimeout) && !memory_may_free_after_flush(1) && !memory_may_free_after_flush(0xFFFFFFFFu), "D: only an acknowledged flush lets the memory be freed");
}
// The real ordering of an import against open / close, as a model over the pure functions: seq0 at entry, the wiring (which may sleep: an event is injected), then under the lock the hello
// and sequence re-checks. Whatever happens while the pages are being wired, an import into a session that is not the one it started in is refused and cleaned up.
namespace {
struct World {
    uint32_t seq = 0, counter = 0; bool hello = false; int wired = 0, cleaned = 0, recorded = 0;
    void open() { seq = ++counter; hello = true; }
    void close() { seq = 0; hello = false; }
    uint32_t import_(const char *event) {
        const uint32_t seq0 = seq;                       // read BEFORE anything is wired
        if (!hello) return kNotReady;
        wired++;                                         // withAddressRange + prepare (may sleep)
        if (event && !std::strcmp(event, "close")) close();
        if (event && !std::strcmp(event, "close+open")) { close(); open(); hello = true; }
        if (event && !std::strcmp(event, "open-nohello")) { close(); open(); hello = false; }
        uint32_t rc = kOk;
        do {
            if (!hello) { rc = kNotReady; break; }
            if (!session_unchanged(seq0, seq)) { rc = kNotReady; break; }
            recorded++;
        } while (false);
        if (rc != kOk) cleaned++;
        return rc;
    }
};
}
static void h7_session_ordering() {
    { World w; w.open(); expect_u("no event: the import is recorded", w.import_(nullptr), kOk); expect(w.recorded == 1 && w.cleaned == 0, "recorded, nothing cleaned"); }
    { World w; w.open(); expect_u("the client closes while the range is wired: refused", w.import_("close"), kNotReady); expect(w.recorded == 0 && w.cleaned == 1 && w.wired == 1, "nothing recorded, the wired range cleaned up"); }
    { World w; w.open(); expect_u("close + a NEW open (hello done) while wired: refused (the sequence changed)", w.import_("close+open"), kNotReady); expect(w.recorded == 0 && w.cleaned == 1, "the new client got nothing recorded"); expect_u("the new session is a different one", w.seq, 2); }
    { World w; w.open(); expect_u("a new open without hello: refused", w.import_("open-nohello"), kNotReady); expect(w.recorded == 0 && w.cleaned == 1, "cleaned"); }
    { World w; expect_u("no session at entry: refused before anything is wired", w.import_(nullptr), kNotReady); expect(w.wired == 0 && w.cleaned == 0, "nothing wired, nothing to clean"); }
    { World w; w.open(); w.close(); expect_u("a closed session stays refused", w.import_(nullptr), kNotReady); }
    // the model's two guards are what the source has; the real ordering is pinned in h5_pins2
}
static void h5_pins2(const std::string &root) {
    const std::string K = root + "/src/navi48-bringup/";
    const std::string eng = slurp(K + "src/amd/native_s1c.cpp"), br = slurp(K + "src/Navi48Bringup.cpp"), hook = slurp(K + "src/apple/AppleHardwareHook.cpp"), engh = slurp(K + "src/amd/native_s1c.h");
    const std::string imp = fn_body(eng, "IOReturn n1c_bo_import_host(");
    expect(!imp.empty(), "n1c_bo_import_host is found (H6 pins)");
    // C: the sequence is read FIRST and re-checked under the lock, before the gate, the cap, the map and the record
    const size_t pSeq = at(imp, "const uint32_t seq0 = ref.id;"), pHello = at(imp, "if (!sess_hello(ref)) return kIOReturnNotReady;"), pWith = at(imp, "IOMemoryDescriptor::withAddressRange("),
                 pLock = at(imp, "IOLockLock(gCliLock);"), pChk = at(imp, "if (!session_unchanged(seq0, s->id)) { rc = kIOReturnNotReady; break; }"),
                 pGate = at(imp, "if (gate != kOk) { rc = (IOReturn)gate; break; }"), pRec = at(imp, "s->bo[h] = b;"), pUnl = at(imp, "IOLockUnlock(gCliLock);"), pTaken = at(imp, "if (!taken) {");
    expect(pSeq != std::string::npos && pChk != std::string::npos, "C: the entry read and the under-lock check exist");
    expect(pSeq < pHello && pHello < pWith, "C: the sequence is read before the Hello check and before any descriptor is created");
    expect(pLock < pChk && pChk < pGate && pGate < pRec && pRec < pUnl, "C: under the lock, after the closed-while-waiting check and BEFORE the gate, the cap, the map and the record");
    expect(pChk > at(imp, "if (s == nullptr || !s->hello) { rc = kIOReturnNotReady; break; }") && pUnl < pTaken, "C: the refusal is inside the locked block, and the !taken cleanup that completes and releases follows it");
    expect_u("C: the session id fixed at entry is compared exactly once, under the lock (0.0.627: the client's own reference)", count_of(imp, "session_unchanged(seq0, s->id)"), 1);
    // A: after collect_pages, before the lock
    const size_t pCol = at(imp, "collect_pages(seg, size, pages, kImportMaxPages)"), pRef = at(imp, "import_first_refused(pages, pre.pages,"), pMsg = at(imp, "import refused: device page");
    expect(pCol != std::string::npos && pRef != std::string::npos && pMsg != std::string::npos && pCol < pRef && pRef < pMsg && pMsg < pLock, "A: the device-page refusal follows collect_pages and precedes the client lock");
    {
        const size_t pB0 = at(imp, "const bool barsOk"); const std::string blk = imp.substr(pB0, pLock - pB0);
        expect(blk.find("return kIOReturnBadArgument;") != std::string::npos && blk.find("md->complete(); md->release();") != std::string::npos && blk.find("IOFree(pages, (vm_size_t)(pre.pages * sizeof(uint64_t)));") != std::string::npos, "A: refused with BadArgument, the never-mapped descriptor completed and released, the page list freed");
        expect(blk.find("const uint64_t bad = import_first_refused(pages, pre.pages, barsOk ? gBars : nullptr, barsOk ? gNBars : 0u, dramTop);\n        if (bad < pre.pages) {\n") != std::string::npos, "A: the FIRST refused page decides: the exact call over the whole list, and the refusal branch is live (bad < pages)");
        expect(blk.find("if (bad < pre.pages) {") != std::string::npos && count_of(blk, "if (false)") == 0 && count_of(blk, "dramTop = 0") == 0, "A: no dead refusal branch, no zero DRAM top");
        expect(blk.find("::n48::hw_hook_ramtop_derived_or_zero()") != std::string::npos, "A: the DRAM top comes from the kext's own EFI-map derivation (0 = unknown)");
        expect(blk.find("barsOk ? gBars : nullptr, barsOk ? gNBars : 0u") != std::string::npos && blk.find("__atomic_load_n(&gBarsState, __ATOMIC_ACQUIRE) == 1u") != std::string::npos, "A: an unlatched BAR list is passed as none (fail closed)");
    }
    expect(eng.find("static PciBar gBars[kMaxPciBars];") != std::string::npos && eng.find("if (!OSCompareAndSwap(0, 2, &gBarsState)) return;") != std::string::npos, "A: the BAR list is latched once (first writer wins)");
    expect(engh.find("void n1c_latch_pci_bars(const uint64_t *base, const uint64_t *size, uint32_t n);") != std::string::npos, "A: the latch is declared");
    {   // where the sizes are read: start(), through the IOPCIDevice, every register, the FULL length (not bar0Size)
        const std::string lb = fn_body(br, "static void latchPciBars(IOPCIDevice *pci) {");
        expect(!lb.empty(), "A: latchPciBars is found");
        for (const char *r : { "kIOPCIConfigBaseAddress0", "kIOPCIConfigBaseAddress1", "kIOPCIConfigBaseAddress2", "kIOPCIConfigBaseAddress3", "kIOPCIConfigBaseAddress4", "kIOPCIConfigBaseAddress5", "kIOPCIConfigExpansionROMBase" })
            expect(lb.find(r) != std::string::npos, (std::string("A: BAR register read: ") + r).c_str());
        expect(lb.find("pci->getDeviceMemoryWithRegister(kRegs[i])") != std::string::npos && lb.find("const uint64_t len = m->getLength();") != std::string::npos && lb.find("bar0Size") == std::string::npos && lb.find("base[n] = (uint64_t)m->getPhysicalAddress(); size[n] = len; n++;") != std::string::npos, "A: the FULL length of each BAR from the IOPCIDevice, never bar0Size");
        expect(lb.find("amdgpu::n1c_latch_pci_bars(base, size, n);") != std::string::npos, "A: handed to the engine");
        expect(br.find("pciDevice->setMemoryEnable(true);\n	latchPciBars(pciDevice);") != std::string::npos, "A: latched in start(), right after the memory space is enabled");
        expect_u("A: latched in exactly one place", count_of(br, "latchPciBars(pciDevice);"), 1);
    }
    expect(hook.find("uint64_t hw_hook_ramtop_derived_or_zero() { return (gRamTopDone != 0u && gRamTopRes.reason == N48_RT_OK) ? gRamTopDerived : 0ull; }") != std::string::npos, "A: the accessor returns the derived top only when the map was read cleanly, else 0");
    // D: bo_release Normal
    const std::string rel = fn_body(eng, "static void bo_release(Session *s, uint32_t h, RelMode mode) {");
    {
        const size_t nrm = at(rel, "} else if (mode == kRelNormal) {"), lk = at(rel, "} else {\n        // Leak:");
        expect(nrm != std::string::npos && lk != std::string::npos, "D: bo_release branches are found");
        const std::string normal = rel.substr(nrm, lk - nrm);
        expect(normal.find("bool memOk = true;") != std::string::npos, "D: a memOk flag starts true");
        expect(normal.find("memOk = memory_may_free_after_flush(flush_vmid(s->vmid)) && memOk;") != std::string::npos && normal.find("memOk = memory_may_free_after_flush(flush_vmid(0)) && memOk;") != std::string::npos, "D: both flushes (the VMID-8 TLB after the unmap, the GART TLB after a fence slot) feed memOk");
        expect(normal.find("else if (b.kind == kBoHost && host_may_release(kHostRelNormal) && memOk) { host_release(b); sys_release(b); }") != std::string::npos, "D: the host pages are completed and released only when memOk");
        expect(normal.find("if (b.kind == kBoGtt) { if (memOk) { sysmem_free(b.sm); sys_release(b); } }") != std::string::npos, "D: the GTT pages are freed only when memOk");
        expect(at(normal, "memOk = memory_may_free_after_flush(flush_vmid(s->vmid))") < at(normal, "host_release(b)") && at(normal, "memOk = memory_may_free_after_flush(flush_vmid(0))") < at(normal, "host_release(b)"), "D: the flush verdicts are taken BEFORE the memory is released");
        expect(normal.find("(void)flush_vmid") == std::string::npos, "D: no flush result is ignored in the Normal branch");
    }
}
static void h6_run(const std::string &root) { h6_device_pages(); h7_session_ordering(); h5_pins2(root); }

int main(int argc, char **argv) {
    const std::string root = argc > 1 ? argv[1] : ".";
    h1_check(); h2_collect(); h3_map(); h4_lifecycle(); h5_pins(root); h6_run(root);
    std::printf("native_hostimport_test: %d checks, %d failed\n", gRun, gFail);
    return gFail == 0 ? 0 : 1;
}
