//
//  gfx12_descriptors.h — GFX12 (Navi 48 / gfx1201) image and sampler descriptors
//
//  The shader side of textures is done: air-to-amdgpu.py lowers Metal's texture
//  operations to llvm.amdgcn.image.*, which take an 8-dword image resource (T#)
//  and a 4-dword sampler (S#) loaded from memory. This file builds those two.
//
//  EVERY field offset, width and enumerated value below is taken from Mesa's
//  machine-readable AMD register database, not from memory or inference:
//
//    src/amd/registers/gfx12-rsrc.json   SQ_IMG_RSRC_WORD0..7, SQ_IMG_SAMP_WORD0..3
//    src/amd/registers/gfx6.json         SQ_RSRC_IMG_TYPE, SQ_SEL_XYZW01,
//                                        SQ_TEX_CLAMP, SQ_TEX_XY_FILTER,
//                                        SQ_TEX_Z_FILTER, SQ_TEX_MIP_FILTER
//    src/amd/common/ac_descriptors.c     ac_build_gfx12_texture_descriptor(),
//                                        ac_build_sampler_descriptor(),
//                                        ac_set_mutable_tex_desc_fields()
//
//  (mesa/mesa @ main, fetched, MIT licensed.)
//
//  Two GFX12-specific encodings are easy to get wrong and are called out where
//  they happen: WIDTH is split across two words, and a LINEAR image's pitch is
//  carried in the DEPTH field rather than a field of its own.
//
//  NOT YET EXERCISED ON HARDWARE. The PC has been down for the whole of this
//  work. Nothing here has run on the 9070 XT.
//

#ifndef NAVI48_GFX12_DESCRIPTORS_H
#define NAVI48_GFX12_DESCRIPTORS_H

#include <stdint.h>

// ---------------------------------------------------------------------------
// Field packing
// ---------------------------------------------------------------------------

// Place `v` at [hi:lo]. Masks to the field width so an out-of-range value
// corrupts only itself instead of silently overwriting its neighbours.
static inline uint32_t gfx12Field(uint32_t v, unsigned lo, unsigned hi)
{
    const unsigned width = hi - lo + 1;
    const uint32_t mask  = (width >= 32) ? 0xFFFFFFFFu : ((1u << width) - 1u);
    return (v & mask) << lo;
}

// ---------------------------------------------------------------------------
// Enumerations (gfx6.json — unchanged through GFX12)
// ---------------------------------------------------------------------------

enum {                              // SQ_RSRC_IMG_TYPE
    kGfx12ImgType1D       = 8,
    kGfx12ImgType2D       = 9,
    kGfx12ImgType3D       = 10,
    kGfx12ImgTypeCube     = 11,
    kGfx12ImgType1DArray  = 12,
    kGfx12ImgType2DArray  = 13,
    kGfx12ImgType2DMSAA   = 14,
    kGfx12ImgType2DMSAAArray = 15,
};

enum {                              // SQ_SEL_XYZW01 — destination channel select
    kGfx12SelZero = 0, kGfx12SelOne = 1,
    kGfx12SelX = 4, kGfx12SelY = 5, kGfx12SelZ = 6, kGfx12SelW = 7,
};

enum {                              // SQ_TEX_CLAMP — sampler address mode
    kGfx12WrapRepeat            = 0,   // SQ_TEX_WRAP
    kGfx12WrapMirror            = 1,
    kGfx12WrapClampToEdge       = 2,   // SQ_TEX_CLAMP_LAST_TEXEL
    kGfx12WrapMirrorOnceToEdge  = 3,
    kGfx12WrapClampHalfBorder   = 4,
    kGfx12WrapMirrorOnceHalfBorder = 5,
    kGfx12WrapClampToBorder     = 6,   // SQ_TEX_CLAMP_BORDER
    kGfx12WrapMirrorOnceBorder  = 7,
};

enum {                              // SQ_TEX_XY_FILTER
    kGfx12FilterPoint = 0, kGfx12FilterLinear = 1,
    kGfx12FilterAnisoPoint = 2, kGfx12FilterAnisoLinear = 3,
};

enum { kGfx12ZFilterNone = 0, kGfx12ZFilterPoint = 1, kGfx12ZFilterLinear = 2 };
enum { kGfx12MipFilterNone = 0, kGfx12MipFilterPoint = 1, kGfx12MipFilterLinear = 2 };

// GFX11_FORMAT (gfx12-rsrc.json). Only the formats Metal's common pixel types
// map onto are listed; the enum is large and an unused entry is a liability.
enum {
    kGfx12Fmt_Invalid          = 0,
    kGfx12Fmt_8_UNORM          = 1,
    kGfx12Fmt_8_UINT           = 5,
    kGfx12Fmt_16_FLOAT         = 13,
    kGfx12Fmt_8_8_UNORM        = 14,
    kGfx12Fmt_32_UINT          = 20,
    kGfx12Fmt_32_FLOAT         = 22,
    kGfx12Fmt_16_16_FLOAT      = 29,
    kGfx12Fmt_8_8_8_8_UNORM    = 42,
    kGfx12Fmt_8_8_8_8_UINT     = 46,
    kGfx12Fmt_32_32_FLOAT      = 50,
    kGfx12Fmt_16_16_16_16_FLOAT= 57,
    kGfx12Fmt_32_32_32_32_FLOAT= 63,
    kGfx12Fmt_8_8_8_8_SRGB     = 66,
};

// Surface swizzle mode. 0 is LINEAR, which is what to use first: it needs no
// tiling maths on either side, and a mismatch between the host's idea of the
// layout and the hardware's is invisible until the image comes out scrambled.
enum { kGfx12SwizzleLinear = 0 };

// ---------------------------------------------------------------------------
// Image resource descriptor (T#) — 8 dwords
// ---------------------------------------------------------------------------

struct Gfx12ImageDesc {
    uint64_t gpuAddress;      // byte address of the image data
    uint32_t width;           // in texels, >= 1
    uint32_t height;          // in texels, >= 1 (1 for 1D)
    uint32_t depth;           // array layers or 3D depth, >= 1
    uint32_t pitchTexels;     // LINEAR only: row stride in texels. 0 = use width
    uint32_t format;          // kGfx12Fmt_*
    uint32_t type;            // kGfx12ImgType*
    uint32_t swizzleMode;     // kGfx12SwizzleLinear
    uint32_t numLevels;       // mip levels, >= 1
    uint32_t dstSel[4];       // kGfx12Sel* per channel; {X,Y,Z,W} for no swizzle
};

// NOTE on mip levels, because two fields carry almost the same thing.
//
// Mesa sets MAX_MIP from `num_levels - 1` and LAST_LEVEL from the view's
// `last_level`, which are different numbers as soon as a view starts above mip
// zero. This builder only makes views that start at BASE_LEVEL 0, so the two
// coincide and both get `numLevels - 1`. A partial-mip view would need them
// separated, and would need BASE_LEVEL set as well — neither is done here,
// because nothing needs it yet and an untested field is worse than an absent
// one.

static inline void gfx12MakeImageDescriptor(const Gfx12ImageDesc *d, uint32_t out[8])
{
    const uint32_t w1 = (d->width  ? d->width  : 1) - 1;
    const uint32_t h1 = (d->height ? d->height : 1) - 1;
    const uint32_t maxMip = (d->numLevels ? d->numLevels : 1) - 1;

    // The base address is stored in 256-byte units, so the image must be
    // 256-byte aligned. ac_set_mutable_tex_desc_fields(): desc[0] = va >> 8,
    // desc[1] |= BASE_ADDRESS_HI(va >> 40).
    const uint64_t va256 = d->gpuAddress >> 8;

    out[0] = (uint32_t)va256;
    out[1] = gfx12Field((uint32_t)(d->gpuAddress >> 40), 0, 7)   // BASE_ADDRESS_HI
           | gfx12Field(maxMip,        12, 16)                   // MAX_MIP
           | gfx12Field(d->format,     17, 24)                   // FORMAT
           | gfx12Field(0,             25, 29)                   // BASE_LEVEL
           // WIDTH is 16 bits split across two words: the low 2 live at the top
           // of WORD1 and the other 14 at the bottom of WORD2. Writing all 16
           // into either one alone gives a texture of the wrong size that still
           // looks plausible for small images.
           | gfx12Field(w1 & 0x3,      30, 31);                  // WIDTH_LO
    out[2] = gfx12Field(w1 >> 2,        0, 13)                   // WIDTH_HI
           | gfx12Field(h1,            14, 29);                  // HEIGHT
    out[3] = gfx12Field(d->dstSel[0],   0,  2)                   // DST_SEL_X
           | gfx12Field(d->dstSel[1],   3,  5)                   // DST_SEL_Y
           | gfx12Field(d->dstSel[2],   6,  8)                   // DST_SEL_Z
           | gfx12Field(d->dstSel[3],   9, 11)                   // DST_SEL_W
           | gfx12Field(maxMip,        15, 19)                   // LAST_LEVEL
           | gfx12Field(d->swizzleMode,20, 24)                   // SW_MODE
           | gfx12Field(d->type,       28, 31);                  // TYPE

    // DEPTH doubles as the pitch for a LINEAR surface with a custom row stride
    // (ac_set_mutable_tex_desc_fields, GFX12 branch): DEPTH holds the low 14
    // bits of pitch-1 and PITCH_MSB the top 2. A linear image whose rows are
    // padded and whose pitch is left at the default is read back sheared.
    uint32_t depthField, pitchMsb = 0;
    if (d->swizzleMode == kGfx12SwizzleLinear && d->pitchTexels &&
        d->pitchTexels != d->width) {
        const uint32_t p1 = d->pitchTexels - 1;
        depthField = p1 & 0x3FFF;
        pitchMsb   = (p1 >> 14) & 0x3;
    } else {
        depthField = (d->depth ? d->depth : 1) - 1;
    }
    out[4] = gfx12Field(depthField,     0, 13)                   // DEPTH
           | gfx12Field(pitchMsb,      14, 15)                   // PITCH_MSB
           | gfx12Field(0,             16, 29);                  // BASE_ARRAY
    out[5] = gfx12Field(4,             20, 22);                  // PERF_MOD = 4
    out[6] = gfx12Field(1,             15, 15);                  // MAX_UNCOMPRESSED_BLOCK_SIZE = 256B
    out[7] = 0;
}

// ---------------------------------------------------------------------------
// Sampler descriptor (S#) — 4 dwords
// ---------------------------------------------------------------------------

struct Gfx12SamplerDesc {
    uint32_t clampX, clampY, clampZ;   // kGfx12Wrap*
    uint32_t magFilter, minFilter;     // kGfx12Filter*
    uint32_t mipFilter;                // kGfx12MipFilter*
    uint32_t unnormalizedCoords;       // 1 = coordinates are in texels
    float    minLod, maxLod;
};

// min/max LOD are unsigned 4.8 fixed point on GFX12 (util_unsigned_fixed(x, 8),
// clamped to [0, 17]).
static inline uint32_t gfx12Fixed4_8(float v)
{
    if (v < 0.0f)  v = 0.0f;
    if (v > 17.0f) v = 17.0f;
    return (uint32_t)(v * 256.0f + 0.5f);
}

static inline void gfx12MakeSamplerDescriptor(const Gfx12SamplerDesc *s, uint32_t out[4])
{
    out[0] = gfx12Field(s->clampX,              0,  2)   // CLAMP_X
           | gfx12Field(s->clampY,              3,  5)   // CLAMP_Y
           | gfx12Field(s->clampZ,              6,  8)   // CLAMP_Z
           | gfx12Field(0,                      9, 11)   // MAX_ANISO_RATIO
           | gfx12Field(0,                     12, 14)   // DEPTH_COMPARE_FUNC
           | gfx12Field(s->unnormalizedCoords, 15, 15)   // FORCE_UNNORMALIZED
           | gfx12Field(1,                     28, 28);  // DISABLE_CUBE_WRAP
    out[1] = gfx12Field(gfx12Fixed4_8(s->minLod),  0, 12)  // MIN_LOD
           | gfx12Field(gfx12Fixed4_8(s->maxLod), 13, 25); // MAX_LOD
    out[2] = gfx12Field(0,                       0, 13)  // LOD_BIAS
           | gfx12Field(s->magFilter,           20, 21)  // XY_MAG_FILTER
           | gfx12Field(s->minFilter,           22, 23)  // XY_MIN_FILTER
           | gfx12Field(kGfx12ZFilterNone,      24, 25)  // Z_FILTER
           | gfx12Field(s->mipFilter,           26, 27)  // MIP_FILTER
           | gfx12Field(1,                      29, 29); // ANISO_OVERRIDE
    out[3] = 0;                                          // BORDER_COLOR_TYPE = 0
}

// The sampler Metal's `texture.read()` implies, and therefore what
// air.get_read_sampler() denotes: unnormalised texel coordinates, no filtering,
// clamp to edge, no mips.
//
// The translator lowers `read` to image.load, which uses no sampler at all, so
// this is needed only where a read path is rebuilt as a sample.
static inline void gfx12MakeReadSampler(uint32_t out[4])
{
    Gfx12SamplerDesc s;
    s.clampX = s.clampY = s.clampZ = kGfx12WrapClampToEdge;
    s.magFilter = s.minFilter = kGfx12FilterPoint;
    s.mipFilter = kGfx12MipFilterNone;
    s.unnormalizedCoords = 1;
    s.minLod = 0.0f;
    s.maxLod = 0.0f;
    gfx12MakeSamplerDescriptor(&s, out);
}

#endif // NAVI48_GFX12_DESCRIPTORS_H
