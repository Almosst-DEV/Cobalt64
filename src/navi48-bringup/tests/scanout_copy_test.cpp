// scanout_copy_test.cpp — prove the scanout geometry check and the row plan refuse every unsafe copy
// (0.0.272). The kext compiles the SAME header.
//
// Build and run on the host Mac (no kernel headers, no hardware, no PC):
//     clang++ -std=c++17 -Wall -Wextra -O1 -I src/navi48-bringup/src/apple \
//         src/navi48-bringup/tests/scanout_copy_test.cpp -o /tmp/scantest && /tmp/scantest
// Also compiled as C by m4-flush.c, so the header must stay C-compatible.

#include <cstdio>
#include <cstdint>

#include "scanout_copy.h"

static int gFail = 0;
static int gRun  = 0;

static void expect_u(const char *what, uint64_t got, uint64_t want)
{
    gRun++;
    if (got != want) {
        gFail++;
        std::printf("FAIL  %-78s got %#llx want %#llx\n", what, (unsigned long long)got, (unsigned long long)want);
    } else {
        std::printf("ok    %-78s %#llx\n", what, (unsigned long long)got);
    }
}

// adopt1's measured values: RDNA4FB Console,* (framebuffer-post.txt), BAR0 0xC0000000 / 256 MiB,
// vramBase 8 MiB and 16304 MiB of VRAM (driverlog psp/gmc lines).
static N48ScanoutGeom live()
{
    N48ScanoutGeom g{};
    g.fbPhys = 0xC0000000ull; g.fbLen = 8294400; g.width = 1920; g.height = 1080; g.rowBytes = 7680;
    g.depth = 32; g.bar0Phys = 0xC0000000ull; g.bar0Size = 256ull << 20; g.vramBase = 8ull << 20;
    g.vramSize = 16304ull << 20;
    return g;
}

int main()
{
    uint64_t off = 1;
    N48ScanoutGeom g = live();
    expect_u("live geometry passes", n48_scanout_geom_check(&g, &off), kScanGeomOk);
    expect_u("live geometry: scanout at VRAM offset 0", off, 0);

    g = live(); g.depth = 24;
    expect_u("24 bpp refused", n48_scanout_geom_check(&g, &off), kScanGeomDepth);
    g = live(); g.fbPhys = 0;
    expect_u("no framebuffer refused", n48_scanout_geom_check(&g, &off), kScanGeomNoFramebuffer);
    g = live(); g.bar0Phys = 0xC0001000ull;
    expect_u("framebuffer below BAR0 refused", n48_scanout_geom_check(&g, &off), kScanGeomOutsideBar0);
    g = live(); g.bar0Size = 4ull << 20;
    expect_u("framebuffer past the mapped BAR0 refused", n48_scanout_geom_check(&g, &off), kScanGeomOutsideBar0);
    g = live(); g.vramBase = 4ull << 20;
    expect_u("framebuffer past the console reservation refused", n48_scanout_geom_check(&g, &off), kScanGeomNotConsole);
    g = live(); g.fbPhys = 0xC0400000ull;   // at 4 MiB: 4 MiB + 7.9 MiB crosses vramBase
    expect_u("framebuffer straddling vramBase refused", n48_scanout_geom_check(&g, &off), kScanGeomNotConsole);
    g = live(); g.rowBytes = 7676;
    expect_u("rowBytes < width*4 refused", n48_scanout_geom_check(&g, &off), kScanGeomShape);
    g = live(); g.height = 1081;
    expect_u("rowBytes*height > length refused", n48_scanout_geom_check(&g, &off), kScanGeomShape);
    g = live(); g.fbLen = 8294402;
    expect_u("misaligned length refused", n48_scanout_geom_check(&g, &off), kScanGeomAlign);

    N48ScanoutPlan p{};
    g = live();
    // The positive control: a 256x64 pattern buffer in the BAR0 pool at VRAM 32 MiB, to (64, 96).
    expect_u("pc plan 256x64 from the BAR0 pool", n48_scanout_plan_rows(&g, 0x2000000, 256 * 64 * 4, 256, 64, 256 * 4,
                                                                         0, 0, 64, 96, 256, 64, 256, &p), kScanPlanOk);
    expect_u("pc plan w", p.w, 256);
    expect_u("pc plan h", p.h, 64);
    expect_u("pc plan row 0 dst = 96*7680 + 64*4", n48_scanout_row_dst(&p, g.rowBytes, 0), 96ull * 7680 + 256);
    expect_u("pc plan row 63 dst", n48_scanout_row_dst(&p, g.rowBytes, 63), 159ull * 7680 + 256);
    expect_u("pc plan row 63 src", n48_scanout_row_src(&p, 63), 0x2000000ull + 63 * 1024);

    // A framebuffer-shaped surface far above BAR0 (Apple's arena), 1920x1080 tight.
    const uint64_t apple = 0x3d7000000ull;
    expect_u("surface band 1920x128 to y=300", n48_scanout_plan_rows(&g, apple, 8294400, 1920, 1080, 7680,
                                                                      0, 0, 0, 300, 1920, 128, 256, &p), kScanPlanOk);
    expect_u("surface band last row dst inside scanout", n48_scanout_row_dst_ok(0, g.fbLen,
             n48_scanout_row_dst(&p, g.rowBytes, 127), p.rowBytes), 1);

    // 0.0.278: the WHOLE FRAME the flush copy now plans (maxRows 2160, packets chunked by the kext), from a staging
    // buffer in the BAR0 pool (tight, stride 7680) to (0,0); every row's destination inside the scanout; and the same frame asked
    // to land at y=1 is clipped to 1079 rows, never past the end.
    expect_u("whole frame 1920x1080 to (0,0), maxRows 2160", n48_scanout_plan_rows(&g, 0x2000000, 8294400, 1920, 1080, 7680,
                                                                                  0, 0, 0, 0, 1920, 1080, 2160, &p), kScanPlanOk);
    expect_u("whole frame plan h", p.h, 1080);
    expect_u("whole frame plan w", p.w, 1920);
    {
        uint32_t okRows = 0;
        for (uint32_t i = 0; i < p.h; i++)
            okRows += n48_scanout_row_dst_ok(0, g.fbLen, n48_scanout_row_dst(&p, g.rowBytes, i), p.rowBytes);
        expect_u("whole frame: all 1080 row destinations inside the scanout", okRows, 1080);
    }
    expect_u("whole frame row 1079 src", n48_scanout_row_src(&p, 1079), 0x2000000ull + 1079ull * 7680);
    expect_u("whole frame at y=1 clipped", n48_scanout_plan_rows(&g, 0x2000000, 8294400, 1920, 1080, 7680,
                                                                 0, 0, 0, 1, 1920, 1080, 2160, &p), kScanPlanOk);
    expect_u("whole frame at y=1 h", p.h, 1079);
    expect_u("whole frame at the old 256-row cap refused", n48_scanout_plan_rows(&g, 0x2000000, 8294400, 1920, 1080, 7680,
                                                                                0, 0, 0, 0, 1920, 1080, 256, &p), kScanPlanTooManyRows);

    // THE OVERLAP REFUSAL: a source allocated over the scanout, or touching it by one dword.
    expect_u("source AT the scanout refused", n48_scanout_plan_rows(&g, 0, 8294400, 1920, 1080, 7680, 0, 0, 0, 300,
                                                                     1920, 128, 256, &p), kScanPlanOverlap);
    expect_u("source ending one dword into the scanout refused",
             n48_scanout_plan_rows(&g, 0, 4, 1, 1, 4, 0, 0, 0, 0, 1, 1, 256, &p), kScanPlanOverlap);
    expect_u("source starting on the scanout's last dword refused",
             n48_scanout_plan_rows(&g, 8294396, 64, 4, 4, 16, 0, 0, 0, 0, 4, 4, 256, &p), kScanPlanOverlap);
    expect_u("source starting exactly at the scanout's end accepted",
             n48_scanout_plan_rows(&g, 8294400, 64, 4, 4, 16, 0, 0, 0, 0, 4, 4, 256, &p), kScanPlanOk);

    // Source range and shape.
    expect_u("source past VRAM refused", n48_scanout_plan_rows(&g, g.vramSize - 8, 64, 4, 4, 16, 0, 0, 0, 0, 4, 4, 256, &p),
             kScanPlanSrcRange);
    expect_u("misaligned source refused", n48_scanout_plan_rows(&g, apple + 2, 64, 4, 4, 16, 0, 0, 0, 0, 4, 4, 256, &p),
             kScanPlanSrcRange);
    expect_u("stride narrower than width refused", n48_scanout_plan_rows(&g, apple, 8294400, 1920, 1080, 7676,
                                                                          0, 0, 0, 0, 16, 16, 256, &p), kScanPlanStride);
    expect_u("shape larger than the allocation refused", n48_scanout_plan_rows(&g, apple, 8294396, 1920, 1080, 7680,
                                                                                0, 0, 0, 0, 16, 16, 256, &p), kScanPlanStride);

    // Clipping.
    expect_u("clip to scanout right edge", n48_scanout_plan_rows(&g, apple, 8294400, 1920, 1080, 7680,
                                                                  0, 0, 1900, 0, 1920, 4, 256, &p), kScanPlanOk);
    expect_u("clipped w = 20", p.w, 20);
    expect_u("clipped last dst dword inside", n48_scanout_row_dst(&p, g.rowBytes, 3) + p.rowBytes, 3ull * 7680 + 1920 * 4);
    expect_u("clip to scanout bottom", n48_scanout_plan_rows(&g, apple, 8294400, 1920, 1080, 7680,
                                                              0, 0, 0, 1070, 16, 64, 256, &p), kScanPlanOk);
    expect_u("clipped h = 10", p.h, 10);
    expect_u("clip to source", n48_scanout_plan_rows(&g, apple, 64 * 64 * 4, 64, 64, 256, 60, 60, 0, 0, 16, 16, 256, &p),
             kScanPlanOk);
    expect_u("source-clipped w = 4", p.w, 4);
    expect_u("destination beyond the scanout is EMPTY", n48_scanout_plan_rows(&g, apple, 8294400, 1920, 1080, 7680,
                                                                              0, 0, 1920, 0, 16, 16, 256, &p), kScanPlanEmpty);
    expect_u("too many rows refused", n48_scanout_plan_rows(&g, apple, 8294400, 1920, 1080, 7680,
                                                             0, 0, 0, 0, 1920, 1080, 256, &p), kScanPlanTooManyRows);
    g.depth = 16;
    expect_u("plan refuses a bad geometry", n48_scanout_plan_rows(&g, apple, 64, 4, 4, 16, 0, 0, 0, 0, 4, 4, 256, &p),
             kScanPlanGeom);

    // Emission check.
    expect_u("row dst ok: whole scanout", n48_scanout_row_dst_ok(0, 8294400, 0, 8294400), 1);
    expect_u("row dst NOT ok: one dword past the end", n48_scanout_row_dst_ok(0, 8294400, 8294400, 4), 0);
    expect_u("row dst NOT ok: straddles the end", n48_scanout_row_dst_ok(0, 8294400, 8294396, 8), 0);
    expect_u("row dst NOT ok: below fbOff", n48_scanout_row_dst_ok(0x1000, 8294400, 0xffc, 4), 0);
    expect_u("row dst NOT ok: zero bytes", n48_scanout_row_dst_ok(0, 8294400, 0, 0), 0);

    // Patterns: known values, distinct rows, distinct bars.
    expect_u("pc pixel (0,0) white bar", n48_pc_pixel(0, 0, 256), 0x00ffffff);
    expect_u("pc pixel (255,5) black bar, row 5 in byte 3", n48_pc_pixel(255, 5, 256), 0x05000000);
    expect_u("pc pixel (32,1) yellow bar", n48_pc_pixel(32, 1, 256), 0x01ffff00);
    expect_u("client pixel (0,0) border", n48_client_pixel(0, 0), 0x00ffffff);
    expect_u("client pixel (4,0) magenta, x in byte 3", n48_client_pixel(4, 0), 0x04ff00ff);
    expect_u("client pixel (300,16) green", n48_client_pixel(300, 16), 0x2c00ff00);

    // Mutation check: the overlap test must be what refuses, not a neighbouring condition. Disable it by
    // moving the scanout away (vramBase check still passes) and confirm the same source is then accepted.
    g = live();
    N48ScanoutGeom moved = g; moved.fbPhys = 0xC0000000ull + 0x10000; moved.vramBase = 16ull << 20;
    expect_u("mutation: the same low source is accepted when the scanout is elsewhere",
             n48_scanout_plan_rows(&moved, 0, 4, 1, 1, 4, 0, 0, 0, 0, 1, 1, 256, &p), kScanPlanOk);

    // --- 0.0.345: the TILED source ------------------------------------------------------------
    // The address equation, checked against the addrlib nibble tables it was read out of
    // (gfx12SwizzlePattern.h: NIBBLE1[2] = {0,0,X0,Y0,X1,Y1,X2,Y2}, NIBBLE2[3] = {Y3,X3,Y4,X4},
    // NIBBLE3[3] = {Y5,X5,Y6,X6}, NIBBLE4[0] = {0,0}). Each case below is ONE bit of x or y, so a
    // transposed or dropped bit fails exactly one line and names itself.
    expect_u("addr3 (0,0)", n48_addr3_64kb_2d_off_4bpe(0, 0, 1920), 0);
    expect_u("addr3 x bit0 -> address bit 2", n48_addr3_64kb_2d_off_4bpe(1, 0, 1920), 1ull << 2);
    expect_u("addr3 y bit0 -> address bit 3", n48_addr3_64kb_2d_off_4bpe(0, 1, 1920), 1ull << 3);
    expect_u("addr3 x bit1 -> address bit 4", n48_addr3_64kb_2d_off_4bpe(2, 0, 1920), 1ull << 4);
    expect_u("addr3 y bit1 -> address bit 5", n48_addr3_64kb_2d_off_4bpe(0, 2, 1920), 1ull << 5);
    expect_u("addr3 x bit2 -> address bit 6", n48_addr3_64kb_2d_off_4bpe(4, 0, 1920), 1ull << 6);
    expect_u("addr3 y bit2 -> address bit 7", n48_addr3_64kb_2d_off_4bpe(0, 4, 1920), 1ull << 7);
    // The parity flip at the 256-byte (8x8 pixel) boundary: Y goes FIRST from address bit 8 up.
    expect_u("addr3 y bit3 -> address bit 8 (NOT x: the parity flips)", n48_addr3_64kb_2d_off_4bpe(0, 8, 1920), 1ull << 8);
    expect_u("addr3 x bit3 -> address bit 9", n48_addr3_64kb_2d_off_4bpe(8, 0, 1920), 1ull << 9);
    expect_u("addr3 y bit4 -> address bit 10", n48_addr3_64kb_2d_off_4bpe(0, 16, 1920), 1ull << 10);
    expect_u("addr3 x bit4 -> address bit 11", n48_addr3_64kb_2d_off_4bpe(16, 0, 1920), 1ull << 11);
    expect_u("addr3 y bit5 -> address bit 12", n48_addr3_64kb_2d_off_4bpe(0, 32, 1920), 1ull << 12);
    expect_u("addr3 x bit5 -> address bit 13", n48_addr3_64kb_2d_off_4bpe(32, 0, 1920), 1ull << 13);
    expect_u("addr3 y bit6 -> address bit 14", n48_addr3_64kb_2d_off_4bpe(0, 64, 1920), 1ull << 14);
    expect_u("addr3 x bit6 -> address bit 15", n48_addr3_64kb_2d_off_4bpe(64, 0, 1920), 1ull << 15);
    // Blocks: 128x128 pixels, row-major, 15 across a 1920-pixel surface.
    expect_u("addr3 block (1,0) at 1920 px", n48_addr3_64kb_2d_off_4bpe(128, 0, 1920), 0x10000);
    expect_u("addr3 block (0,1) at 1920 px is 15 blocks on", n48_addr3_64kb_2d_off_4bpe(0, 128, 1920), 15ull << 16);
    expect_u("addr3 last pixel of block (0,0)", n48_addr3_64kb_2d_off_4bpe(127, 127, 1920), 0xfffc);
    expect_u("addr3 (1919,1079) inside the allocation", n48_addr3_64kb_2d_off_4bpe(1919, 1079, 1920) < 0x870000, 1);
    // The surface size closes on the live allocation exactly (: 0x870000 = 135 x 64 KiB).
    expect_u("addr3 bytes 1920x1080", n48_addr3_64kb_2d_bytes_4bpe(1920, 1080), 0x870000);
    expect_u("addr3 bytes 1920x1152 is the SAME 9 block rows", n48_addr3_64kb_2d_bytes_4bpe(1920, 1152), 0x870000);
    expect_u("addr3 bytes 256x256 is 4 blocks", n48_addr3_64kb_2d_bytes_4bpe(256, 256), 0x40000);
    // The equation is a PERMUTATION over one block: 16384 distinct offsets, all inside 64 KiB.
    {
        static unsigned char seen[65536 / 4];
        unsigned dup = 0, oob = 0;
        for (unsigned y = 0; y < 128; y++)
            for (unsigned x = 0; x < 128; x++) {
                const uint64_t o = n48_addr3_64kb_2d_off_4bpe(x, y, 128);
                if (o >= 65536 || (o & 3u)) { oob++; continue; }
                if (seen[o / 4]++) dup++;
            }
        expect_u("addr3 one block: no duplicate offset", dup, 0);
        expect_u("addr3 one block: nothing outside 64 KiB or misaligned", oob, 0);
    }

    // The tiled plan. Apple's live surface: 1920x1080 at VRAM 0x10930000, 0x870000 bytes, swizzle 3.
    N48ScanoutTiledPlan t{};
    g = live();
    expect_u("tiled plan: the live WindowServer surface", n48_scanout_plan_tiled(&g, 0x10930000, 0x870000, 1920, 1080,
             3, 0, 0, 0, 0, 1920, 1080, &t), kScanTiledOk);
    expect_u("tiled plan: linear pitch in ELEMENTS", t.linPitch, 1920);
    expect_u("tiled plan: element size log2 at 32 bpp", t.elementLog2, 2);
    expect_u("tiled plan: rect", ((uint64_t)t.w << 16) | t.h, (1920ull << 16) | 1080);
    expect_u("tiled plan: destination check passes", n48_scanout_tiled_dst_ok(&t, g.fbLen), 1);
    expect_u("tiled plan: a linear surface is REFUSED (no equation for it)",
             n48_scanout_plan_tiled(&g, 0x10930000, 0x870000, 1920, 1080, 0, 0, 0, 0, 0, 1920, 1080, &t), kScanTiledSwizzle);
    expect_u("tiled plan: gfx10's own 27 is REFUSED (it is not the gfx12 enum)",
             n48_scanout_plan_tiled(&g, 0x10930000, 0x870000, 1920, 1080, 27, 0, 0, 0, 0, 1920, 1080, &t), kScanTiledSwizzle);
    expect_u("tiled plan: a base that is not 64 KiB aligned is REFUSED",
             n48_scanout_plan_tiled(&g, 0x10930000 + 0x1000, 0x870000, 1920, 1080, 3, 0, 0, 0, 0, 1920, 1080, &t), kScanTiledAlign);
    expect_u("tiled plan: an allocation shorter than the tiled surface is REFUSED",
             n48_scanout_plan_tiled(&g, 0x10930000, 0x860000, 1920, 1080, 3, 0, 0, 0, 0, 1920, 1080, &t), kScanTiledSrcRange);
    expect_u("tiled plan: a source over the scanout is REFUSED",
             n48_scanout_plan_tiled(&g, 0, 0x870000, 1920, 1080, 3, 0, 0, 0, 0, 1920, 1080, &t), kScanTiledOverlap);
    expect_u("tiled plan: a source past VRAM is REFUSED",
             n48_scanout_plan_tiled(&g, g.vramSize - 0x10000, 0x870000, 1920, 1080, 3, 0, 0, 0, 0, 1920, 1080, &t),
             kScanTiledSrcRange);
    expect_u("tiled plan: a destination outside the scanout is EMPTY",
             n48_scanout_plan_tiled(&g, 0x10930000, 0x870000, 1920, 1080, 3, 0, 0, 1920, 0, 1920, 1080, &t), kScanTiledEmpty);
    expect_u("tiled plan: clips to the scanout bottom", n48_scanout_plan_tiled(&g, 0x10930000, 0x870000, 1920, 1080, 3,
             0, 0, 0, 1000, 1920, 1080, &t), kScanTiledOk);
    expect_u("tiled plan: clipped height", t.h, 80);
    expect_u("tiled plan: the clipped rectangle still passes the destination check",
             n48_scanout_tiled_dst_ok(&t, g.fbLen), 1);
    expect_u("tiled plan: a sub-window at a tiled offset", n48_scanout_plan_tiled(&g, 0x10930000, 0x870000, 1920, 1080,
             3, 128, 128, 0, 0, 256, 256, &t), kScanTiledOk);
    expect_u("tiled plan: sub-window origin kept", ((uint64_t)t.srcX << 16) | t.srcY, (128ull << 16) | 128);
    // The emission check, on its own.
    t = N48ScanoutTiledPlan{}; t.linPitch = 1920; t.dstX = 0; t.dstY = 0; t.w = 1920; t.h = 1080;
    expect_u("tiled dst ok: the whole scanout", n48_scanout_tiled_dst_ok(&t, 8294400), 1);
    t.h = 1081;
    expect_u("tiled dst NOT ok: one row past the end", n48_scanout_tiled_dst_ok(&t, 8294400), 0);
    t.h = 1080; t.dstX = 1;
    expect_u("tiled dst NOT ok: shifted right by one pixel", n48_scanout_tiled_dst_ok(&t, 8294400), 0);
    t.dstX = 0; t.w = 1921;
    expect_u("tiled dst NOT ok: wider than the pitch", n48_scanout_tiled_dst_ok(&t, 8294400), 0);
    t.w = 1920; t.h = 0;
    expect_u("tiled dst NOT ok: zero rows", n48_scanout_tiled_dst_ok(&t, 8294400), 0);

    std::printf("\n%d/%d passed\n", gRun - gFail, gRun);
    return gFail ? 1 : 0;
}
