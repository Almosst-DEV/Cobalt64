//
//  test_gfx12_descriptors.cpp — runs on the Mac, not the GPU.
//
//  This cannot prove the bit positions match the hardware; only the 9070 XT can
//  do that. What it CAN prove is the thing most likely to be wrong in a hand-
//  written bitfield packer: that a value written to one field does not spill
//  into a neighbour, and that every field lands where the register database
//  says it does.
//
//  Build:  c++ -std=c++17 -O1 -o /tmp/t test_gfx12_descriptors.cpp && /tmp/t
//

#include "../src/amd/gfx12_descriptors.h"
#include "../src/amd/gfx12_formats.h"
#include <cstdio>
#include <cstring>

static int failures = 0;

static void expectEq(const char *what, uint32_t got, uint32_t want)
{
    if (got != want) {
        printf("  FAIL %-34s got 0x%08x want 0x%08x\n", what, got, want);
        failures++;
    }
}

static uint32_t bits(uint32_t w, unsigned lo, unsigned hi)
{
    const unsigned n = hi - lo + 1;
    return (w >> lo) & ((n >= 32) ? 0xFFFFFFFFu : ((1u << n) - 1u));
}

// Which bits of the 8-dword image descriptor change when one input changes.
static void changedBits(const Gfx12ImageDesc &a, const Gfx12ImageDesc &b,
                        uint32_t mask[8])
{
    uint32_t x[8], y[8];
    gfx12MakeImageDescriptor(&a, x);
    gfx12MakeImageDescriptor(&b, y);
    for (int i = 0; i < 8; i++) mask[i] = x[i] ^ y[i];
}

static Gfx12ImageDesc baseImage()
{
    Gfx12ImageDesc d;
    memset(&d, 0, sizeof(d));
    d.gpuAddress = 0;
    d.width = 1; d.height = 1; d.depth = 1;
    d.pitchTexels = 0;
    d.format = kGfx12Fmt_Invalid;
    d.type = 0;
    d.swizzleMode = kGfx12SwizzleLinear;
    d.numLevels = 1;
    d.dstSel[0] = d.dstSel[1] = d.dstSel[2] = d.dstSel[3] = 0;
    return d;
}

int main()
{
    printf("gfx12 descriptor packing\n");

    // ---- image: every field reads back from its documented range ----------
    {
        Gfx12ImageDesc d = baseImage();
        d.gpuAddress = 0x0000ABCDEF123400ull;   // 256-byte aligned
        d.width  = 1920;
        d.height = 1080;
        d.depth  = 6;
        d.format = kGfx12Fmt_8_8_8_8_UNORM;
        d.type   = kGfx12ImgType2DArray;
        d.numLevels = 4;
        d.dstSel[0] = kGfx12SelX; d.dstSel[1] = kGfx12SelY;
        d.dstSel[2] = kGfx12SelZ; d.dstSel[3] = kGfx12SelW;

        uint32_t o[8];
        gfx12MakeImageDescriptor(&d, o);

        expectEq("BASE_ADDRESS_LO", o[0], (uint32_t)(d.gpuAddress >> 8));
        expectEq("BASE_ADDRESS_HI", bits(o[1], 0, 7),
                 (uint32_t)(d.gpuAddress >> 40) & 0xFF);
        expectEq("MAX_MIP",   bits(o[1], 12, 16), 3);
        expectEq("FORMAT",    bits(o[1], 17, 24), kGfx12Fmt_8_8_8_8_UNORM);
        expectEq("BASE_LEVEL",bits(o[1], 25, 29), 0);

        // WIDTH is 16 bits split 2 + 14 across two words.
        const uint32_t w1 = 1920 - 1;
        const uint32_t widthBack = bits(o[1], 30, 31) | (bits(o[2], 0, 13) << 2);
        expectEq("WIDTH (recombined)", widthBack, w1);
        expectEq("HEIGHT",   bits(o[2], 14, 29), 1080 - 1);

        expectEq("DST_SEL_X", bits(o[3], 0, 2), kGfx12SelX);
        expectEq("DST_SEL_Y", bits(o[3], 3, 5), kGfx12SelY);
        expectEq("DST_SEL_Z", bits(o[3], 6, 8), kGfx12SelZ);
        expectEq("DST_SEL_W", bits(o[3], 9, 11), kGfx12SelW);
        expectEq("LAST_LEVEL",bits(o[3], 15, 19), 3);
        expectEq("SW_MODE",   bits(o[3], 20, 24), kGfx12SwizzleLinear);
        expectEq("TYPE",      bits(o[3], 28, 31), kGfx12ImgType2DArray);

        expectEq("DEPTH",     bits(o[4], 0, 13), 6 - 1);
        expectEq("BASE_ARRAY",bits(o[4], 16, 29), 0);
        expectEq("PERF_MOD",  bits(o[5], 20, 22), 4);
        expectEq("MAX_UNCOMPRESSED_BLOCK_SIZE", bits(o[6], 15, 15), 1);
        expectEq("WORD7",     o[7], 0);
    }

    // ---- the linear-pitch special case ------------------------------------
    {
        Gfx12ImageDesc d = baseImage();
        d.width = 100; d.height = 50; d.depth = 1;
        d.pitchTexels = 128;                     // padded rows
        uint32_t o[8];
        gfx12MakeImageDescriptor(&d, o);
        const uint32_t p1 = 128 - 1;
        const uint32_t back = bits(o[4], 0, 13) | (bits(o[4], 14, 15) << 14);
        expectEq("PITCH via DEPTH+PITCH_MSB", back, p1);
    }

    // ---- no field spills into another -------------------------------------
    // One input at a time; the bits that move must be exactly the documented
    // range for that field and nothing else.
    struct Probe { const char *name; int word; unsigned lo, hi;
                   void (*set)(Gfx12ImageDesc &); };
    static const Probe probes[] = {
        {"MAX_MIP/LAST_LEVEL", -1, 0, 0, [](Gfx12ImageDesc &d){ d.numLevels = 8; }},
        {"FORMAT",     1, 17, 24, [](Gfx12ImageDesc &d){ d.format = 0xFF; }},
        {"HEIGHT",     2, 14, 29, [](Gfx12ImageDesc &d){ d.height = 0xFFFF + 1; }},
        {"TYPE",       3, 28, 31, [](Gfx12ImageDesc &d){ d.type = 0xF; }},
        {"SW_MODE",    3, 20, 24, [](Gfx12ImageDesc &d){ d.swizzleMode = 0x1F; }},
        {"DST_SEL_W",  3,  9, 11, [](Gfx12ImageDesc &d){ d.dstSel[3] = 7; }},
        {"DEPTH",      4,  0, 13, [](Gfx12ImageDesc &d){ d.depth = 0x3FFF + 1; }},
    };
    for (const Probe &p : probes) {
        if (p.word < 0) continue;                 // spans two fields by design
        Gfx12ImageDesc a = baseImage(), b = baseImage();
        p.set(b);
        uint32_t mask[8];
        changedBits(a, b, mask);
        const unsigned n = p.hi - p.lo + 1;
        const uint32_t want = (((n >= 32) ? 0xFFFFFFFFu : ((1u << n) - 1u)) << p.lo);
        for (int i = 0; i < 8; i++) {
            const uint32_t expect = (i == p.word) ? want : 0u;
            if (mask[i] != expect) {
                printf("  FAIL %-20s word%d changed 0x%08x, expected 0x%08x\n",
                       p.name, i, mask[i], expect);
                failures++;
            }
        }
    }

    // ---- sampler -----------------------------------------------------------
    {
        Gfx12SamplerDesc s;
        s.clampX = kGfx12WrapRepeat;
        s.clampY = kGfx12WrapClampToEdge;
        s.clampZ = kGfx12WrapClampToBorder;
        s.magFilter = kGfx12FilterLinear;
        s.minFilter = kGfx12FilterPoint;
        s.mipFilter = kGfx12MipFilterLinear;
        s.unnormalizedCoords = 0;
        s.minLod = 0.0f; s.maxLod = 15.0f;
        uint32_t o[4];
        gfx12MakeSamplerDescriptor(&s, o);
        expectEq("CLAMP_X", bits(o[0], 0, 2), kGfx12WrapRepeat);
        expectEq("CLAMP_Y", bits(o[0], 3, 5), kGfx12WrapClampToEdge);
        expectEq("CLAMP_Z", bits(o[0], 6, 8), kGfx12WrapClampToBorder);
        expectEq("FORCE_UNNORMALIZED", bits(o[0], 15, 15), 0);
        expectEq("MIN_LOD", bits(o[1], 0, 12), 0);
        expectEq("MAX_LOD", bits(o[1], 13, 25), 15 * 256);
        expectEq("XY_MAG_FILTER", bits(o[2], 20, 21), kGfx12FilterLinear);
        expectEq("XY_MIN_FILTER", bits(o[2], 22, 23), kGfx12FilterPoint);
        expectEq("MIP_FILTER",    bits(o[2], 26, 27), kGfx12MipFilterLinear);
    }

    // ---- the read sampler --------------------------------------------------
    {
        uint32_t o[4];
        gfx12MakeReadSampler(o);
        expectEq("read CLAMP_X", bits(o[0], 0, 2), kGfx12WrapClampToEdge);
        expectEq("read FORCE_UNNORMALIZED", bits(o[0], 15, 15), 1);
        expectEq("read XY_MAG_FILTER", bits(o[2], 20, 21), kGfx12FilterPoint);
        expectEq("read MIP_FILTER", bits(o[2], 26, 27), kGfx12MipFilterNone);
    }

    // ---- 4.8 fixed point ---------------------------------------------------
    expectEq("fixed4_8(0)",    gfx12Fixed4_8(0.0f), 0);
    expectEq("fixed4_8(1)",    gfx12Fixed4_8(1.0f), 256);
    expectEq("fixed4_8(0.5)",  gfx12Fixed4_8(0.5f), 128);
    expectEq("fixed4_8(clamp)",gfx12Fixed4_8(99.0f), 17 * 256);

    // ---- the shader's decode must invert the host's encode -----------------
    // air-to-amdgpu.py answers air.get_width_texture_2d and friends by pulling
    // the fields back out of this descriptor with a fixed sequence of shifts
    // and masks. Those shifts are reproduced here EXACTLY as the translator
    // emits them (TEXDIM_RE handling in lower_more_ops), so a change to either
    // side that breaks the pairing fails here rather than on the GPU.
    {
        const uint32_t sizes[][3] = {
            {1, 1, 1}, {2, 3, 4}, {256, 256, 1}, {1920, 1080, 1},
            {4096, 4096, 64}, {16384, 16384, 2048}, {65536, 65536, 16384},
        };
        for (const auto &sz : sizes) {
            Gfx12ImageDesc d = baseImage();
            d.width = sz[0]; d.height = sz[1]; d.depth = sz[2];
            d.type = kGfx12ImgType2DArray;
            d.numLevels = 5;
            uint32_t o[8];
            gfx12MakeImageDescriptor(&d, o);

            // width  : (WORD1[31:30]) | (WORD2[13:0] << 2), then +1
            const uint32_t w = ((o[1] >> 30) | ((o[2] & 16383u) << 2)) + 1;
            // height : (WORD2 >> 14) & 0xFFFF, then +1
            const uint32_t h = ((o[2] >> 14) & 65535u) + 1;
            // depth  : WORD4 & 0x3FFF, then +1
            const uint32_t z = (o[4] & 16383u) + 1;
            // mips   : (WORD1 >> 12) & 0x1F, then +1
            const uint32_t m = ((o[1] >> 12) & 31u) + 1;

            char label[64];
            snprintf(label, sizeof label, "decode width %u", sz[0]);
            expectEq(label, w, sz[0]);
            snprintf(label, sizeof label, "decode height %u", sz[1]);
            expectEq(label, h, sz[1]);
            snprintf(label, sizeof label, "decode depth %u", sz[2]);
            expectEq(label, z, sz[2]);
            snprintf(label, sizeof label, "decode mips (%u)", sz[0]);
            expectEq(label, m, 5);
        }
    }

    // ---- the Metal format table -------------------------------------------
    {
        uint32_t f; uint32_t sel[4];
        // RGBA8Unorm (70) and BGRA8Unorm (80) are the SAME hardware format
        // distinguished only by the channel select. If the swizzle ever stopped
        // being applied, both would still "work" and one of them would come out
        // with red and blue exchanged — the kind of wrong that looks like a
        // colour-management bug for a week.
        expectEq("RGBA8Unorm found", gfx12FormatForMetal(70, &f, sel), 1);
        expectEq("RGBA8Unorm format", f, kGfx12Fmt_8_8_8_8_UNORM);
        expectEq("RGBA8Unorm sel.x", sel[0], kGfx12SelX);
        expectEq("RGBA8Unorm sel.z", sel[2], kGfx12SelZ);
        expectEq("BGRA8Unorm found", gfx12FormatForMetal(80, &f, sel), 1);
        expectEq("BGRA8Unorm format", f, kGfx12Fmt_8_8_8_8_UNORM);
        expectEq("BGRA8Unorm sel.x", sel[0], kGfx12SelZ);
        expectEq("BGRA8Unorm sel.z", sel[2], kGfx12SelX);
        // Single-channel formats must report 0,0,1 for the channels the texture
        // does not have, not garbage.
        expectEq("R32Float found", gfx12FormatForMetal(55, &f, sel), 1);
        expectEq("R32Float format", f, kGfx12Fmt_32_FLOAT);
        expectEq("R32Float sel.y", sel[1], kGfx12SelZero);
        expectEq("R32Float sel.w", sel[3], kGfx12SelOne);
        // An unmapped format must be refused, not approximated.
        expectEq("unknown format refused", gfx12FormatForMetal(0xBEEF, &f, sel), 0);
        // Every entry must be usable: no duplicate Metal values, and the
        // selects must be real enumerators.
        const unsigned n = sizeof(kGfx12FormatTable) / sizeof(kGfx12FormatTable[0]);
        for (unsigned i = 0; i < n; i++) {
            for (unsigned j = i + 1; j < n; j++)
                if (kGfx12FormatTable[i].metalFormat == kGfx12FormatTable[j].metalFormat) {
                    printf("  FAIL duplicate Metal format %u\n",
                           kGfx12FormatTable[i].metalFormat);
                    failures++;
                }
            for (int c = 0; c < 4; c++) {
                const uint32_t v = kGfx12FormatTable[i].dstSel[c];
                if (v > 7 || v == 2 || v == 3) {
                    printf("  FAIL format %u channel %d has select %u\n",
                           kGfx12FormatTable[i].metalFormat, c, v);
                    failures++;
                }
            }
        }
    }

    printf(failures ? "\n%d FAILURE(S)\n" : "\nall packing checks passed\n", failures);
    return failures != 0;
}
