// gen_fixture_cycle80_r5.cpp - build 0.0.525 (switch 80): the HELPER of tests/gen_fixture_cycle80.py. It runs the KEXT'S OWN
// R5′ builder (gfx_dep.h n48_r5_build_ib + n48_r5_resolve, the pair gfxsrc_decide_frame runs, with the kext's cap 512 and
// N48_GCAP_F_FILLER walk) over run11c's captured IB bodies, so the fixture's per-frame colour targets and memory destinations are
// the ones the kext would have recorded - not a second scanner's. Not a suite: the generator compiles and runs it.
//     clang++ -std=c++17 -O1 -I src/navi48-bringup/src/apple src/navi48-bringup/tests/gen_fixture_cycle80_r5.cpp -o <scratch>/r5
// stdin: `M <page va hex> <page hex>` (the VA -> VRAM page map from capdec's regions), then per frame `F <f> <nib>` followed by
// nib lines `I <ib va hex> <dwords> <path>`. stdout per frame: `R <f> <bucket> <ntgt> <nmemw> <walk_ok> <targets_ok> <memw_ok>
// <scan_over> <has_dispatch>` then `T <va> <page>` / `W <va> <page>` lines, then cyc515's write set over
// the same bodies (gfx_cycle515.h n48_cy_ws_scan: every CB/DB base ANY IB sets) as `Y <n> <unknown> <over>` + `S <va>` lines. A VA whose page is not in the map resolves to a
// synthetic page (the VA's page with bit 62 set): it can never equal a real VRAM page, so it can only miss a layer, and the
// map holds every layer page (X, X' are CB0 of captured frames).
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>
#include "gfx_dep.h"
#include "gfx_cycle515.h"

static std::map<uint64_t, uint64_t> gMap;
static uint32_t resolve_cb(void *, uint64_t va, uint64_t *page)
{
    const uint64_t p = va & ~0xFFFull;
    auto it = gMap.find(p);
    *page = it != gMap.end() ? it->second : (p | (1ull << 62));
    return 1u;
}
static std::vector<uint32_t> read_bin(const char *path)
{
    std::vector<uint32_t> w;
    FILE *f = std::fopen(path, "rb");
    if (!f) return w;
    uint32_t x;
    while (std::fread(&x, 4, 1, f) == 1) w.push_back(x);
    std::fclose(f);
    return w;
}
static n48_gcap_item gItems[512];
static void emit(uint32_t fno, uint32_t nib, const std::vector<std::pair<uint64_t, std::string>> &ibs)
{
    static n48_r5_frame x;
    std::memset(&x, 0, sizeof x);
    x.ib_ok = nib >= 1u ? 1u : 0u; x.walk_ok = x.ib_ok; x.targets_ok = x.ib_ok; x.memw_ok = x.ib_ok; x.ib_over = 0u;
    uint32_t so = 0u; uint64_t mi = 0ull;
    static n48_cy cy;
    n48_cy_frame_begin(&cy, 1u);
    for (const auto &ib : ibs) {
        const std::vector<uint32_t> w = read_bin(ib.second.c_str());
        if (w.empty()) { x.ib_ok = 0u; x.walk_ok = 0u; continue; }
        n48_cy_ws_scan(&cy, w.data(), (uint32_t)w.size());   // cyc515's write set: EVERY CB/DB base any IB sets
        uint32_t so1 = 0u;
        n48_r5_build_ib(&x, w.data(), (uint32_t)w.size(), gItems, 512u, 0x400000000ull, N48_GCAP_F_FILLER, &so1, &mi);
        if (so1) so = 1u;
    }
    n48_r5_resolve(&x, resolve_cb, nullptr);
    std::printf("R %u %u %u %u %u %u %u %u %u\n", fno, n48_r5_bucket(&x), x.ntgt, x.nmemw, x.walk_ok, x.targets_ok, x.memw_ok,
                so, x.has_dispatch);
    for (uint32_t i = 0; i < x.ntgt && i < N48_CP_TGT_MAX; i++)
        std::printf("T %llx %llx\n", (unsigned long long)x.tgt[i].va, (unsigned long long)x.tgt[i].page);
    for (uint32_t i = 0; i < x.nmemw && i < N48_CP_MEMW_MAX; i++)
        std::printf("W %llx %llx\n", (unsigned long long)x.memw[i].va, (unsigned long long)x.memw[i].page);
    std::printf("Y %u %u %u\n", cy.nws, cy.ws_unknown, cy.ws_over);
    for (uint32_t i = 0; i < cy.nws && i < N48_CY_WS_MAX; i++) std::printf("S %llx\n", (unsigned long long)cy.ws[i]);
}
int main()
{
    char line[4096];
    uint32_t fno = 0u, nib = 0u;
    std::vector<std::pair<uint64_t, std::string>> ibs;
    bool have = false;
    while (std::fgets(line, sizeof line, stdin)) {
        unsigned long long a = 0, b = 0; unsigned u = 0, v = 0; char path[3000];
        if (line[0] == 'M' && std::sscanf(line + 1, "%llx %llx", &a, &b) == 2) gMap[a] = b;
        else if (line[0] == 'F' && std::sscanf(line + 1, "%u %u", &u, &v) == 2) {
            if (have) emit(fno, nib, ibs);
            fno = u; nib = v; ibs.clear(); have = true;
        } else if (line[0] == 'I' && std::sscanf(line + 1, "%llx %u %2999s", &a, &u, path) == 3) ibs.push_back({ a, path });
    }
    if (have) emit(fno, nib, ibs);
    return 0;
}
