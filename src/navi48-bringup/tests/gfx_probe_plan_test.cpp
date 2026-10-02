// gfx_probe_plan_test.cpp — the copy-back probe's target choice (0.0.283).
//     clang++ -std=c++17 -Wall -Wextra -O1 -fsanitize=address,undefined -I src/navi48-bringup/src/apple \
//         src/navi48-bringup/tests/gfx_probe_plan_test.cpp -o /tmp/probetest && /tmp/probetest [CAPDIR]
// With CAPDIR (tools/m4-xlat/capdecode.py output of gfxcap1) the test also runs over SecurityAgent's real IBs and checks the
// targets read from them: F2 0x4000a0000 366x113, F9 0x400005000 26x78, F7 0x4000a0000 51x110; F5 IB0 has no colour target.
#include <cstdio>
#include <cstdint>
#include <vector>
#include "gfx_probe_plan.h"

static int gFail = 0, gRun = 0;
static void expect_u(const char *what, uint64_t got, uint64_t want)
{
    gRun++;
    if (got != want) { gFail++; std::printf("FAIL  %-66s got %#llx want %#llx\n", what, (unsigned long long)got, (unsigned long long)want); }
    else std::printf("ok    %-66s %#llx\n", what, (unsigned long long)got);
}

static std::vector<uint32_t> load(const char *path)
{
    std::vector<uint32_t> v;
    FILE *f = std::fopen(path, "rb");
    if (!f) return v;
    uint32_t w;
    while (std::fread(&w, 4, 1, f) == 1) v.push_back(w);
    std::fclose(f);
    return v;
}

int main(int argc, char **argv)
{
    n48_probe_target t; uint32_t why = 99;
    // Synthetic: a 16F pass, then the final pass: CB0 BASE 0x04000a00 (VA 0x4000a0000), INFO 0x8824, scissor 366x113, mask 0xf, draw.
    std::vector<uint32_t> ib = {
        0xC0016900, 0x00000318, 0x04001100,             // CB_COLOR0_BASE 0x400110000
        0xC0016900, 0x0000031c, 0x10040714,             // INFO RG16F
        0xC0026900, 0x0000000c, 0x00000000, 0x00800180, // scissor 384x128
        0xC0016900, 0x0000008e, 0x0000000f,             // CB_TARGET_MASK
        0xC0042700, 0xffffffff, 0x00001200, 0x00000004, 0x00000040, 0x00000000,   // DRAW_INDEX_2
        0xC0016900, 0x00000318, 0x04000a00,
        0xC0016900, 0x00000390, 0x00000000,
        0xC0016900, 0x0000031c, 0x00008824,
        0xC0026900, 0x0000000c, 0x00000000, 0x0071016e,
        0x80000000,
        0xC0012D00, 0x00000004, 0x00000002,             // DRAW_INDEX_AUTO
    };
    expect_u("synthetic: found", n48_probe_last_target(ib.data(), (uint32_t)ib.size(), &t, &why), 1);
    expect_u("  why OK", why, N48_PROBE_OK);
    expect_u("  VA = base << 8", t.va, 0x4000a0000ull);
    expect_u("  info", t.info, 0x8824);
    expect_u("  width 366", t.width, 366);
    expect_u("  height 113", t.height, 113);
    expect_u("  draws 2", t.draws, 2);
    expect_u("  magenta for 2-10-10-10", n48_probe_magenta(t.info), 0xFFF003FFu);
    expect_u("  magenta for 8-8-8-8", n48_probe_magenta(0x8828), 0xFFFF00FFu);
    // Refusals, each on a copy with one thing changed.
    { auto c = ib; c.resize(c.size() - 3);           // drop the final draw: the last draw is the RG16F one
      expect_u("last draw RG16F -> FORMAT refusal", n48_probe_last_target(c.data(), (uint32_t)c.size(), &t, &why), 0);
      expect_u("  why FORMAT", why, N48_PROBE_FORMAT); }
    { auto c = ib; c[12] = 0x00000020;               // mask enables target 1 only
      expect_u("mask without target 0 -> refusal", n48_probe_last_target(c.data(), (uint32_t)c.size(), &t, &why), 0);
      expect_u("  why MASK", why, N48_PROBE_MASK); }
    { auto c = ib; c[30] = 0x00010001;               // scissor TL not at the origin
      expect_u("scissor TL (1,1) -> refusal", n48_probe_last_target(c.data(), (uint32_t)c.size(), &t, &why), 0);
      expect_u("  why SCISSOR", why, N48_PROBE_SCISSOR); }
    { static const uint32_t nodraw[] = { 0xC0016900, 0x00000318, 0x04000a00 };
      expect_u("no draw -> refusal", n48_probe_last_target(nodraw, 3, &t, &why), 0);
      expect_u("  why NO_DRAW", why, N48_PROBE_NO_DRAW); }
    { static const uint32_t notarget[] = { 0xC0012D00, 0x00000003, 0x00000002 };
      expect_u("draw with no CB base -> refusal", n48_probe_last_target(notarget, 3, &t, &why), 0);
      expect_u("  why NO_TARGET", why, N48_PROBE_NO_TARGET); }
    if (argc > 1) {
        char p[512];
        struct { const char *f; uint64_t va; uint32_t w, h, ok; } real[] = {
            { "F00002-IB0.bin", 0x4000a0000ull, 366, 113, 1 }, { "F00009-IB0.bin", 0x400005000ull, 26, 78, 1 },
            { "F00007-IB0.bin", 0x4000a0000ull, 51, 110, 1 }, { "F00005-IB0.bin", 0, 0, 0, 0 } };
        for (auto &r : real) {
            std::snprintf(p, sizeof p, "%s/ib/%s", argv[1], r.f);
            auto v = load(p);
            char w[128];
            std::snprintf(w, sizeof w, "real %s loaded", r.f); expect_u(w, v.empty() ? 0 : 1, 1);
            const uint32_t ok = n48_probe_last_target(v.data(), (uint32_t)v.size(), &t, &why);
            std::snprintf(w, sizeof w, "real %s found", r.f); expect_u(w, ok, r.ok);
            if (r.ok) {
                std::snprintf(w, sizeof w, "real %s VA", r.f); expect_u(w, t.va, r.va);
                std::snprintf(w, sizeof w, "real %s size", r.f); expect_u(w, (uint64_t)t.width << 16 | t.height, (uint64_t)r.w << 16 | r.h);
                std::snprintf(w, sizeof w, "real %s info 0x8824", r.f); expect_u(w, t.info, 0x8824);
            }
        }
    }
    std::printf("%d/%d passed\n", gRun - gFail, gRun);
    return gFail ? 1 : 0;
}
