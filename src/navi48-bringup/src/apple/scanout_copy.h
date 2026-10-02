// scanout_copy.h — milestone 4 route c' wall 3 (0.0.272): the PURE half of the copy into
// RDNA4FB's scanout. No kernel headers, no hardware: the kext, the host test
// (tests/scanout_copy_test.cpp) and the PC test client (tools/pc/m4-flush.c) compile this same file.
//
// What it decides:
//   1. n48_scanout_geom_check - is the framebuffer RDNA4FB describes a scanout we may write at all:
//      32 bpp, inside BAR0, wholly inside the console reservation below the bring-up region (VRAM
//      [0, vramBase) - nothing of ours or Apple's is ever allocated there), and self-consistent.
//   2. n48_scanout_plan_rows - one COPY_LINEAR per row from a source buffer in VRAM into a rectangle of
//      the scanout, CLIPPED to both, REFUSED when the source range overlaps the scanout (a VRAM
//      allocation placed over the scanout would make the copy read what it writes), when the stride
//      is narrower than the width, or when anything leaves the VRAM the card has.
//   3. n48_scanout_row_dst_ok - the per-packet check the kext repeats at emission: a destination range
//      must lie inside [fbOff, fbOff + fbLen). "Never write outside 0xC0000000 + 8 MiB" is this function.
//   4. The two patterns: the kext's positive-control bars and the test client's stripes, so the kext
//      can recognise a client pixel it did not write.
#ifndef N48_SCANOUT_COPY_H
#define N48_SCANOUT_COPY_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Refusal reasons of n48_scanout_geom_check (0 = usable).
enum {
    kScanGeomOk            = 0,
    kScanGeomNoFramebuffer = 1,   // base or length zero
    kScanGeomDepth         = 2,   // not 32 bpp
    kScanGeomOutsideBar0   = 3,   // base below BAR0 or [base, base+len) past the mapped BAR0
    kScanGeomNotConsole    = 4,   // not wholly inside VRAM [0, vramBase) - the console reservation
    kScanGeomShape         = 5,   // width*4 > rowBytes, or rowBytes*height > length, or zero dims
    kScanGeomAlign         = 6,   // base, length or rowBytes not dword-aligned
};

typedef struct N48ScanoutGeom {
    uint64_t fbPhys;      // RDNA4FB "Console,BaseAddress" (0xC0000000 on this card)
    uint64_t fbLen;       // RDNA4FB "Console,Length"      (8294400)
    uint32_t width;       // "Console,Width"   1920
    uint32_t height;      // "Console,Height"  1080
    uint32_t rowBytes;    // "Console,RowBytes" 7680
    uint32_t depth;       // "Console,Depth"   32
    uint64_t bar0Phys;    // our IOPCIDevice BAR0 physical base (a DIFFERENT source than fbPhys)
    uint64_t bar0Size;    // bytes of BAR0 mapped by the kext
    uint64_t vramBase;    // byte offset of the bring-up region; [0, vramBase) is the console reservation
    uint64_t vramSize;    // RCC_CONFIG_MEMSIZE bytes
} N48ScanoutGeom;

static inline uint32_t n48_scanout_geom_check(const N48ScanoutGeom *g, uint64_t *fbOffOut)
{
    if (fbOffOut) *fbOffOut = 0;
    if (!g || g->fbPhys == 0 || g->fbLen == 0) return kScanGeomNoFramebuffer;
    if (g->depth != 32) return kScanGeomDepth;
    if ((g->fbPhys & 3u) || (g->fbLen & 3u) || (g->rowBytes & 3u)) return kScanGeomAlign;
    if (g->fbPhys < g->bar0Phys) return kScanGeomOutsideBar0;
    const uint64_t off = g->fbPhys - g->bar0Phys;
    if (off > g->bar0Size || g->fbLen > g->bar0Size - off) return kScanGeomOutsideBar0;
    if (off > g->vramBase || g->fbLen > g->vramBase - off) return kScanGeomNotConsole;
    if (g->width == 0 || g->height == 0 || g->rowBytes == 0) return kScanGeomShape;
    if ((uint64_t)g->width * 4u > g->rowBytes) return kScanGeomShape;
    if ((uint64_t)g->rowBytes * g->height > g->fbLen) return kScanGeomShape;
    if (fbOffOut) *fbOffOut = off;
    return kScanGeomOk;
}

// Refusal reasons of n48_scanout_plan_rows (0 = planned).
enum {
    kScanPlanOk          = 0,
    kScanPlanGeom        = 1,   // the geometry did not pass n48_scanout_geom_check
    kScanPlanSrcRange    = 2,   // source empty, misaligned, or past the card's VRAM
    kScanPlanOverlap     = 3,   // source range intersects the scanout range - REFUSED
    kScanPlanStride      = 4,   // srcStride < srcWidth*4, or the source shape does not fit srcLen
    kScanPlanEmpty       = 5,   // nothing left after clipping to source and scanout
    kScanPlanTooManyRows = 6,   // more rows than the caller's packet budget
    kScanPlanRowBytes    = 7,   // one row longer than COPY_LINEAR's 22-bit count
};

typedef struct N48ScanoutPlan {
    uint64_t fbOff;        // scanout's VRAM byte offset
    uint64_t srcOff;       // source's VRAM byte offset
    uint32_t srcStride;
    uint32_t srcX, srcY;   // clipped source origin
    uint32_t dstX, dstY;   // clipped destination origin
    uint32_t w, h;         // clipped size in pixels
    uint32_t rowBytes;     // bytes per copied row = w*4
} N48ScanoutPlan;

#define N48_SDMA_COPY_MAX_BYTES 0x00400000u   // COPY_LINEAR byte_count-1 is 22 bits (amdgpu_sdma.h)

static inline uint64_t n48_scanout_row_src(const N48ScanoutPlan *p, uint32_t i)
{
    return p->srcOff + (uint64_t)(p->srcY + i) * p->srcStride + (uint64_t)p->srcX * 4u;
}

static inline uint64_t n48_scanout_row_dst(const N48ScanoutPlan *p, uint32_t fbRowBytes, uint32_t i)
{
    return p->fbOff + (uint64_t)(p->dstY + i) * fbRowBytes + (uint64_t)p->dstX * 4u;
}

// The emission-time check: [dst, dst+bytes) inside [fbOff, fbOff + fbLen).
static inline int n48_scanout_row_dst_ok(uint64_t fbOff, uint64_t fbLen, uint64_t dst, uint64_t bytes)
{
    if (bytes == 0 || dst < fbOff) return 0;
    const uint64_t rel = dst - fbOff;
    return rel <= fbLen && bytes <= fbLen - rel;
}

// Plan copying the rectangle (srcX, srcY, w, h) of a source buffer (VRAM [srcOff, srcOff+srcLen),
// srcWidth x srcHeight pixels, srcStride bytes per row) to (dstX, dstY) in the scanout.
static inline uint32_t n48_scanout_plan_rows(const N48ScanoutGeom *g,
                                             uint64_t srcOff, uint64_t srcLen,
                                             uint32_t srcWidth, uint32_t srcHeight, uint32_t srcStride,
                                             uint32_t srcX, uint32_t srcY,
                                             uint32_t dstX, uint32_t dstY,
                                             uint32_t w, uint32_t h, uint32_t maxRows,
                                             N48ScanoutPlan *out)
{
    uint64_t fbOff = 0;
    if (!out) return kScanPlanGeom;
    out->fbOff = 0; out->srcOff = 0; out->srcStride = 0;
    out->srcX = out->srcY = out->dstX = out->dstY = out->w = out->h = out->rowBytes = 0;
    if (n48_scanout_geom_check(g, &fbOff) != kScanGeomOk) return kScanPlanGeom;
    if (srcLen == 0 || (srcOff & 3u) || srcOff > g->vramSize || srcLen > g->vramSize - srcOff)
        return kScanPlanSrcRange;
    // The overlap refusal is on the WHOLE source allocation, not only the rows copied: an allocation
    // that shares any byte with the scanout is not a buffer we may treat as independent of it.
    if (srcOff < fbOff + g->fbLen && fbOff < srcOff + srcLen) return kScanPlanOverlap;
    if (srcWidth == 0 || srcHeight == 0 || srcStride == 0 || (srcStride & 3u) ||
        (uint64_t)srcWidth * 4u > srcStride)
        return kScanPlanStride;
    // The last row needs only width*4 bytes, not a full stride.
    if ((uint64_t)(srcHeight - 1) * srcStride + (uint64_t)srcWidth * 4u > srcLen) return kScanPlanStride;
    // Clip to the source.
    if (srcX >= srcWidth || srcY >= srcHeight) return kScanPlanEmpty;
    if (w > srcWidth - srcX) w = srcWidth - srcX;
    if (h > srcHeight - srcY) h = srcHeight - srcY;
    // Clip to the scanout.
    if (dstX >= g->width || dstY >= g->height) return kScanPlanEmpty;
    if (w > g->width - dstX) w = g->width - dstX;
    if (h > g->height - dstY) h = g->height - dstY;
    if (w == 0 || h == 0) return kScanPlanEmpty;
    if (h > maxRows) return kScanPlanTooManyRows;
    if ((uint64_t)w * 4u > N48_SDMA_COPY_MAX_BYTES) return kScanPlanRowBytes;
    out->fbOff = fbOff; out->srcOff = srcOff; out->srcStride = srcStride;
    out->srcX = srcX; out->srcY = srcY; out->dstX = dstX; out->dstY = dstY;
    out->w = w; out->h = h; out->rowBytes = w * 4u;
    // Belt and braces: every row the plan names must pass the emission check.
    {
        uint32_t i;
        for (i = 0; i < h; i++) {
            if (!n48_scanout_row_dst_ok(fbOff, g->fbLen, n48_scanout_row_dst(out, g->rowBytes, i), out->rowBytes))
                return kScanPlanEmpty;
            if (n48_scanout_row_src(out, i) + out->rowBytes > srcOff + srcLen) return kScanPlanStride;
        }
    }
    return kScanPlanOk;
}

// --- the TILED source (0.0.345) ------------------------------------------------------------
//
// CONFIRMED the composited surface is tiled, so n48_scanout_row_src above is meaningless for it and the
// row plan cannot be used on that path. The tiled path plans ONE SDMA COPY_TILED_SUB_WINDOW packet for the
// whole frame instead: a tiled source at (srcX,srcY) inside a surfW x surfH surface, into the linear scanout
// at (dstX,dstY). Same refusals as the row plan (overlap, range, dword alignment, destination inside the
// scanout), plus the two this packet adds: a swizzle mode we are willing to emit, and every value fitting the
// field it goes in.
//
// n48_addr3_64kb_2d_off_4bpe is the gfx12 ADDR3_64KB_2D element address at 4 BYTES PER ELEMENT, CONFIRMED
// from vendored addrlib, not guessed:
//   re/graphics/src/mesa/src/amd/addrlib/src/gfx12/gfx12SwizzlePattern.h:95-102
//       GFX12_SW_64KB_2D_1xAA_PATINFO, the 4-BPE row, = { 2, 3, 3, 0 }
//   ...:195-221 GFX12_SW_PATTERN_NIBBLE1[2] = { 0, 0, X0, Y0, X1, Y1, X2, Y2 }   -> address bits 0..7
//   ...:224-240 GFX12_SW_PATTERN_NIBBLE2[3] = { Y3, X3, Y4, X4 }                 -> address bits 8..11
//   ...:242-258 GFX12_SW_PATTERN_NIBBLE3[3] = { Y5, X5, Y6, X6 }                 -> address bits 12..15
//   ...:260-273 GFX12_SW_PATTERN_NIBBLE4[0] = { 0, 0 }                           -> nothing above bit 15
// So the 64 KiB block is 128 x 128 pixels (7 X bits, 7 Y bits, exactly addrlib's ComputeThinBlockDimension
// answer in) and the within-block order is NOT plain Morton: bits 2..7 interleave X FIRST
// (X0 Y0 X1 Y1 X2 Y2) and bits 8..15 interleave Y FIRST (Y3 X3 Y4 X4 Y5 X5 Y6 X6). The parity flips at the
// 256-byte (8 x 8 pixel) boundary. Blocks themselves are row-major across the surface.
// This function is what mode 5 lays a KNOWN TILED PATTERN down with, so a detile that disagrees with it names
// the disagreement per position; the same run then has the hardware tile the verified linear result back and
// compares, which is what turns "our equation" into "the hardware's layout".
static inline uint64_t n48_addr3_64kb_2d_off_4bpe(uint32_t x, uint32_t y, uint32_t pitchPx)
{
    const uint32_t lx = x & 127u, ly = y & 127u;
    const uint32_t blocksPerRow = (pitchPx + 127u) >> 7;
    uint32_t o = 0;
    o |= (lx & 1u) << 2;          o |= (ly & 1u) << 3;
    o |= ((lx >> 1) & 1u) << 4;   o |= ((ly >> 1) & 1u) << 5;
    o |= ((lx >> 2) & 1u) << 6;   o |= ((ly >> 2) & 1u) << 7;
    o |= ((ly >> 3) & 1u) << 8;   o |= ((lx >> 3) & 1u) << 9;
    o |= ((ly >> 4) & 1u) << 10;  o |= ((lx >> 4) & 1u) << 11;
    o |= ((ly >> 5) & 1u) << 12;  o |= ((lx >> 5) & 1u) << 13;
    o |= ((ly >> 6) & 1u) << 14;  o |= ((lx >> 6) & 1u) << 15;
    return ((uint64_t)(y >> 7) * blocksPerRow + (x >> 7)) * 65536ull + o;
}

// Bytes a 64 KiB-block 2D surface of surfW x surfH elements occupies (blocks are 128 x 128 at 4 bpe).
static inline uint64_t n48_addr3_64kb_2d_bytes_4bpe(uint32_t surfW, uint32_t surfH)
{
    return (uint64_t)((surfW + 127u) >> 7) * (uint64_t)((surfH + 127u) >> 7) * 65536ull;
}

// Refusal reasons of n48_scanout_plan_tiled (0 = planned).
enum {
    kScanTiledOk        = 0,
    kScanTiledGeom      = 1,   // the geometry did not pass n48_scanout_geom_check
    kScanTiledSrcRange  = 2,   // source empty, misaligned, shorter than the tiled surface, or past VRAM
    kScanTiledOverlap   = 3,   // source range intersects the scanout range - REFUSED
    kScanTiledShape     = 4,   // zero dims, or the rect leaves the source surface
    kScanTiledEmpty     = 5,   // nothing left after clipping to source and scanout
    kScanTiledSwizzle   = 6,   // a swizzle mode this driver will not emit
    kScanTiledDstRange  = 7,   // the destination rectangle is not wholly inside the scanout
    kScanTiledAlign     = 8,   // the tiled base is not 64 KiB aligned
    kScanTiledField     = 9,   // a value does not fit its packet field
};

typedef struct N48ScanoutTiledPlan {
    uint64_t fbOff;          // linear destination surface base = the scanout's VRAM byte offset
    uint64_t srcOff;         // tiled source surface base, VRAM byte offset
    uint32_t surfW, surfH;   // tiled surface extent in ELEMENTS (Apple's CB_COLOR0_ATTRIB2 MIP0_WIDTH/HEIGHT + 1)
    uint32_t swizzle;        // gfx12 ADDR3 enum for the info dword (3 = ADDR3_64KB_2D)
    uint32_t elementLog2;    // log2(bytes per element); 2 at 32 bpp
    uint32_t srcX, srcY;     // tiled sub-window origin
    uint32_t dstX, dstY;     // linear sub-window origin, pixels into the scanout
    uint32_t linPitch;       // linear pitch in ELEMENTS
    uint32_t linSlice;       // linear slice pitch in ELEMENTS
    uint32_t w, h;           // rectangle, pixels
} N48ScanoutTiledPlan;

// The emission-time destination check for a tiled sub-window copy: EVERY byte the packet may write, which is
// rows dstY..dstY+h-1, columns dstX..dstX+w-1 of a linPitch-element linear surface based at fbOff.
static inline int n48_scanout_tiled_dst_ok(const N48ScanoutTiledPlan *p, uint64_t fbLen)
{
    uint64_t first, last;
    if (!p || p->w == 0 || p->h == 0 || p->linPitch == 0) return 0;
    if ((uint64_t)p->dstX + p->w > p->linPitch) return 0;
    first = ((uint64_t)p->dstY * p->linPitch + p->dstX) * 4u;
    last  = ((uint64_t)(p->dstY + p->h - 1u) * p->linPitch + p->dstX + p->w) * 4u;
    return last > first && last <= fbLen;
}

static inline uint32_t n48_scanout_plan_tiled(const N48ScanoutGeom *g,
                                              uint64_t srcOff, uint64_t srcLen,
                                              uint32_t surfW, uint32_t surfH, uint32_t swizzle,
                                              uint32_t srcX, uint32_t srcY,
                                              uint32_t dstX, uint32_t dstY,
                                              uint32_t w, uint32_t h,
                                              N48ScanoutTiledPlan *out)
{
    uint64_t fbOff = 0, need = 0;
    if (!out) return kScanTiledGeom;
    out->fbOff = 0; out->srcOff = 0; out->surfW = 0; out->surfH = 0; out->swizzle = 0;
    out->elementLog2 = 0; out->srcX = 0; out->srcY = 0; out->dstX = 0; out->dstY = 0;
    out->linPitch = 0; out->linSlice = 0; out->w = 0; out->h = 0;
    if (n48_scanout_geom_check(g, &fbOff) != kScanGeomOk) return kScanTiledGeom;
    // Only the one mode this driver has an address equation and a self-test for.
    if (swizzle != 3u) return kScanTiledSwizzle;
    if (srcLen == 0 || (srcOff & 3u) || srcOff > g->vramSize || srcLen > g->vramSize - srcOff)
        return kScanTiledSrcRange;
    if (srcOff & 0xffffu) return kScanTiledAlign;                     // 64 KiB blocks need a 64 KiB base
    if (srcOff < fbOff + g->fbLen && fbOff < srcOff + srcLen) return kScanTiledOverlap;
    if (surfW == 0 || surfH == 0) return kScanTiledShape;
    need = n48_addr3_64kb_2d_bytes_4bpe(surfW, surfH);
    if (need > srcLen) return kScanTiledSrcRange;
    // Clip to the source surface, then to the scanout. No silent growth: only shrink.
    if (srcX >= surfW || srcY >= surfH) return kScanTiledEmpty;
    if (w > surfW - srcX) w = surfW - srcX;
    if (h > surfH - srcY) h = surfH - srcY;
    if (dstX >= g->width || dstY >= g->height) return kScanTiledEmpty;
    if (w > g->width - dstX) w = g->width - dstX;
    if (h > g->height - dstY) h = g->height - dstY;
    if (w == 0 || h == 0) return kScanTiledEmpty;
    out->fbOff = fbOff; out->srcOff = srcOff;
    out->surfW = surfW; out->surfH = surfH; out->swizzle = swizzle; out->elementLog2 = 2u;
    out->srcX = srcX; out->srcY = srcY; out->dstX = dstX; out->dstY = dstY;
    out->linPitch = g->rowBytes / 4u;
    out->linSlice = (uint32_t)((uint64_t)out->linPitch * g->height);
    out->w = w; out->h = h;
    // Every field must fit. surf w/h and the rect go into 16-bit (value-1) fields; the linear pitch too.
    if (surfW > 0x10000u || surfH > 0x10000u || w > 0x10000u || h > 0x10000u ||
        out->linPitch == 0 || out->linPitch > 0x10000u ||
        srcX > 0xffffu || srcY > 0xffffu || dstX > 0xffffu || dstY > 0xffffu)
        return kScanTiledField;
    if (!n48_scanout_tiled_dst_ok(out, g->fbLen)) return kScanTiledDstRange;
    return kScanTiledOk;
}

// --- build 0.0.518 (flip mode F1): EXACTLY TWO DESTINATIONS --------------------------------------------
//
// n48_scanout_geom_check admits one destination, the console g describes ("A", VRAM [0, vramBase)). Flip mode adds ONE more:
// the registered back buffer "B" = VRAM [bOff, bOff + bLen), console-shaped (bLen >= rowBytes * height), inside BAR0, above
// vramBase (so never the console reservation and never overlapping A), 64 KiB aligned. n48_scanout_flip_dst_check admits
// dstOff == A's offset (every n48_scanout_geom_check rule) or dstOff == bOff (B's rules); ANY other destination is
// kScanGeomNotConsole, exactly as a non-console framebuffer is. *dstLenOut is the admitted destination's length.
static inline uint32_t n48_scanout_flip_dst_check(const N48ScanoutGeom *g, uint64_t bOff, uint64_t bLen, uint64_t dstOff,
                                                  uint64_t *dstLenOut)
{
    uint64_t aOff = 0;
    if (dstLenOut) *dstLenOut = 0;
    const uint32_t r = n48_scanout_geom_check(g, &aOff);
    if (r != kScanGeomOk) return r;
    if (dstOff == aOff) { if (dstLenOut) *dstLenOut = g->fbLen; return kScanGeomOk; }
    if (dstOff != bOff || bLen == 0) return kScanGeomNotConsole;
    if ((bOff & 0xffffull) || (bLen & 3u)) return kScanGeomAlign;
    if (bOff < g->vramBase) return kScanGeomNotConsole;
    if (bOff < aOff + g->fbLen && aOff < bOff + bLen) return kScanGeomNotConsole;
    if (bOff > g->bar0Size || bLen > g->bar0Size - bOff) return kScanGeomOutsideBar0;
    if ((uint64_t)g->rowBytes * g->height > bLen) return kScanGeomShape;
    if (dstLenOut) *dstLenOut = bLen;
    return kScanGeomOk;
}

// The tiled plan into A or B. n48_scanout_plan_tiled's rules, in its order, against the console (its geometry, its clip, the
// source's overlap with A), plus the destination admitted by n48_scanout_flip_dst_check (exactly A or B; anything else is
// kScanTiledGeom), the source's overlap with THAT destination, and every byte the packet may write inside it. For dstOff == A the
// answer and the plan equal n48_scanout_plan_tiled's (tests/gfx_flipmode_test.cpp T7 pins that differentially).
// Written out rather than calling n48_scanout_plan_tiled so that function keeps exactly one caller in the kext's translation
// unit (navi48_scanout_copy_tiled), where the compiler inlines it: the 0.0.517 present path's code and frame stay as they were.
static inline uint32_t n48_scanout_plan_tiled_to(const N48ScanoutGeom *g, uint64_t bOff, uint64_t bLen, uint64_t dstOff,
                                                 uint64_t srcOff, uint64_t srcLen, uint32_t surfW, uint32_t surfH,
                                                 uint32_t swizzle, uint32_t srcX, uint32_t srcY, uint32_t dstX, uint32_t dstY,
                                                 uint32_t w, uint32_t h, N48ScanoutTiledPlan *out)
{
    uint64_t fbOff = 0, need = 0, dstLen = 0;
    if (!out) return kScanTiledGeom;
    out->fbOff = 0; out->srcOff = 0; out->surfW = 0; out->surfH = 0; out->swizzle = 0;
    out->elementLog2 = 0; out->srcX = 0; out->srcY = 0; out->dstX = 0; out->dstY = 0;
    out->linPitch = 0; out->linSlice = 0; out->w = 0; out->h = 0;
    if (n48_scanout_geom_check(g, &fbOff) != kScanGeomOk) return kScanTiledGeom;
    if (n48_scanout_flip_dst_check(g, bOff, bLen, dstOff, &dstLen) != kScanGeomOk) return kScanTiledGeom;
    if (swizzle != 3u) return kScanTiledSwizzle;
    if (srcLen == 0 || (srcOff & 3u) || srcOff > g->vramSize || srcLen > g->vramSize - srcOff)
        return kScanTiledSrcRange;
    if (srcOff & 0xffffu) return kScanTiledAlign;
    if (srcOff < fbOff + g->fbLen && fbOff < srcOff + srcLen) return kScanTiledOverlap;
    if (srcOff < dstOff + dstLen && dstOff < srcOff + srcLen) return kScanTiledOverlap;
    if (surfW == 0 || surfH == 0) return kScanTiledShape;
    need = n48_addr3_64kb_2d_bytes_4bpe(surfW, surfH);
    if (need > srcLen) return kScanTiledSrcRange;
    if (srcX >= surfW || srcY >= surfH) return kScanTiledEmpty;
    if (w > surfW - srcX) w = surfW - srcX;
    if (h > surfH - srcY) h = surfH - srcY;
    if (dstX >= g->width || dstY >= g->height) return kScanTiledEmpty;
    if (w > g->width - dstX) w = g->width - dstX;
    if (h > g->height - dstY) h = g->height - dstY;
    if (w == 0 || h == 0) return kScanTiledEmpty;
    out->fbOff = dstOff; out->srcOff = srcOff;
    out->surfW = surfW; out->surfH = surfH; out->swizzle = swizzle; out->elementLog2 = 2u;
    out->srcX = srcX; out->srcY = srcY; out->dstX = dstX; out->dstY = dstY;
    out->linPitch = g->rowBytes / 4u;
    out->linSlice = (uint32_t)((uint64_t)out->linPitch * g->height);
    out->w = w; out->h = h;
    if (surfW > 0x10000u || surfH > 0x10000u || w > 0x10000u || h > 0x10000u ||
        out->linPitch == 0 || out->linPitch > 0x10000u ||
        srcX > 0xffffu || srcY > 0xffffu || dstX > 0xffffu || dstY > 0xffffu)
        return kScanTiledField;
    if (!n48_scanout_tiled_dst_ok(out, dstLen)) return kScanTiledDstRange;
    return kScanTiledOk;
}

// --- the patterns ------------------------------------------------------------------------------------
// Pixels are little-endian 32-bit words, as the scanout stores them. Byte 3 is not displayed.
//
// Positive control (kext-filled VRAM): eight vertical bars, full-intensity colours, with the row index
// in the undisplayed byte so every row is distinct and a row-order error is visible in a readback.
static inline uint32_t n48_pc_pixel(uint32_t x, uint32_t y, uint32_t w)
{
    static const uint32_t bars[8] = { 0x00ffffffu, 0x00ffff00u, 0x0000ffffu, 0x0000ff00u,
                                      0x00ff00ffu, 0x00ff0000u, 0x000000ffu, 0x00000000u };
    const uint32_t b = w ? (x * 8u) / w : 0u;
    return bars[b & 7u] | ((y & 0xffu) << 24);
}

// 0.0.345: the DETILE self-test's pattern. One UNIQUE value per position, and the value IS the
// position: a wrong dword anywhere in the destination names the source pixel it actually came from, so a
// failure is a map, not a count. Valid for surfaces up to 65536 x 65536.
static inline uint32_t n48_tile_probe_pixel(uint32_t x, uint32_t y)
{
    return ((y & 0xffffu) << 16) | (x & 0xffffu);
}

// D7 (0.0.417, notes/design/SDMA-DCC-NOPTE.md) — THE UNIFORM LINEAR PROBE `accel scanout 8` writes, and the
// value every sampled position must then read back. A uniform (constant) 256-byte block is exactly what DCC
// detects: with our SDMA's no-PTE write compression ON the block is STORED as a constant code and the raw MM
// window reads that code, not this value; with `sdmadcc 1` the write is raw and this value comes back. The
// same helper is what the host test pins (tests/scanout_selftest_test.cpp).
#define N48_TILE_UNIFORM_PIXEL 0xFF00FF00u
static inline uint32_t n48_tile_uniform_pixel(uint32_t x, uint32_t y)
{
    (void)x; (void)y;
    return N48_TILE_UNIFORM_PIXEL;
}

// build 0.0.514 B3/B4: a live framebuffer dimension (RDNA4FB's Console,Width/Height; 0 = absent) or the
// old 1080p constant. A value above 16384 is treated as absent (no surface this driver drives is that large).
static inline uint32_t n48_live_dim(uint32_t live, uint32_t fallback)
{
    return (live && live <= 16384u) ? live : fallback;
}

// --- S5 (notes/design/SCANOUT-SELFTEST-FULL.md, 0.0.414): THE BOUNDED SAMPLE SET ------------------------
//
// The full-geometry self-test samples a surface at every pixel of a handful of named rows and columns plus a
// 64x64 grid, so a wrong copy is named by position without reading every one of two million pixels. The set
// is DEDUPLICATED BY CONSTRUCTION: a grid point is skipped when its row is a named row or its column a named
// column, and a column sample is skipped when its row is a named row. Out-of-surface named rows/columns are
// dropped, so the SAME generator serves the 256x256 control and the 1920x1080 live geometry. Pure: the kext
// (Navi48Bringup.cpp) and the host test (tests/scanout_selftest_test.cpp) compile this file.
// build 0.0.514 B3: the 1440p edge row 1439 and column 2559 are APPENDED (the lists stay ascending, so
// every row/column below a surface's edge is still a prefix): out-of-surface entries are dropped, so the 256x256 control's
// and the 1920x1080 case's sample sets are 0.0.513's exactly (tests/scanout_selftest_test.cpp pins both).
#define N48_TILE_SS_ROWS 10u
#define N48_TILE_SS_COLS 5u
#define N48_TILE_SS_GRID 64u

static inline uint32_t n48_tile_ss_row(uint32_t i)
{
    static const uint32_t r[N48_TILE_SS_ROWS] = { 0u, 1u, 7u, 8u, 127u, 128u, 1023u, 1024u, 1079u, 1439u };
    return i < N48_TILE_SS_ROWS ? r[i] : 0u;
}
static inline uint32_t n48_tile_ss_col(uint32_t i)
{
    static const uint32_t c[N48_TILE_SS_COLS] = { 0u, 127u, 128u, 1919u, 2559u };
    return i < N48_TILE_SS_COLS ? c[i] : 0u;
}
static inline int n48_tile_ss_row_of(uint32_t y)
{
    uint32_t i;
    for (i = 0; i < N48_TILE_SS_ROWS; i++) if (n48_tile_ss_row(i) == y) return 1;
    return 0;
}
static inline int n48_tile_ss_col_of(uint32_t x)
{
    uint32_t i;
    for (i = 0; i < N48_TILE_SS_COLS; i++) if (n48_tile_ss_col(i) == x) return 1;
    return 0;
}
static inline uint32_t n48_tile_ss_rows_in(uint32_t h)
{
    uint32_t i, n = 0;
    for (i = 0; i < N48_TILE_SS_ROWS; i++) if (n48_tile_ss_row(i) < h) n++;
    return n;
}
static inline uint32_t n48_tile_ss_cols_in(uint32_t w)
{
    uint32_t i, n = 0;
    for (i = 0; i < N48_TILE_SS_COLS; i++) if (n48_tile_ss_col(i) < w) n++;
    return n;
}
static inline uint32_t n48_tile_ss_grid_x(uint32_t cx, uint32_t w)
{
    return (uint32_t)(((uint64_t)cx * w) / N48_TILE_SS_GRID);
}
static inline uint32_t n48_tile_ss_grid_y(uint32_t cy, uint32_t h)
{
    return (uint32_t)(((uint64_t)cy * h) / N48_TILE_SS_GRID);
}
// The k-th value in [0,h) that is NOT a named row (used for the column samples' y).
static inline uint32_t n48_tile_ss_nonrow(uint32_t h, uint32_t k)
{
    uint32_t y;
    for (y = 0; y < h; y++) {
        if (n48_tile_ss_row_of(y)) continue;
        if (k == 0) return y;
        k--;
    }
    return h ? h - 1u : 0u;
}
// How many samples the set holds for a w x h surface.
static inline uint64_t n48_tile_ss_count(uint32_t w, uint32_t h)
{
    uint32_t cy, cx;
    uint32_t geR = 0, geC = 0;
    if (w == 0 || h == 0) return 0;
    {
        const uint32_t R = n48_tile_ss_rows_in(h), C = n48_tile_ss_cols_in(w);
        const uint64_t rows = (uint64_t)R * w;
        const uint64_t cols = (uint64_t)C * ((uint64_t)h - R);
        for (cy = 0; cy < N48_TILE_SS_GRID; cy++) if (n48_tile_ss_row_of(n48_tile_ss_grid_y(cy, h))) geR++;
        for (cx = 0; cx < N48_TILE_SS_GRID; cx++) if (n48_tile_ss_col_of(n48_tile_ss_grid_x(cx, w))) geC++;
        return rows + cols +
               (uint64_t)(N48_TILE_SS_GRID - geR) * (uint64_t)(N48_TILE_SS_GRID - geC);
    }
}
// The i-th sample (i < count): row samples, then column samples, then grid points. Returns 0 when out of range.
static inline int n48_tile_ss_at(uint32_t w, uint32_t h, uint64_t i, uint32_t *px, uint32_t *py)
{
    uint32_t cy, cx, k;
    const uint32_t R = n48_tile_ss_rows_in(h), C = n48_tile_ss_cols_in(w);
    const uint64_t rows = (uint64_t)R * w;
    if (px) *px = 0;
    if (py) *py = 0;
    if (w == 0 || h == 0) return 0;
    if (i < rows) {
        if (px) *px = (uint32_t)(i % w);
        if (py) *py = n48_tile_ss_row((uint32_t)(i / w));
        return 1;
    }
    i -= rows;
    {
        const uint64_t percol = (uint64_t)h - R;
        const uint64_t cols = (uint64_t)C * percol;
        if (i < cols) {
            k = percol ? (uint32_t)(i % percol) : 0u;
            if (px) *px = n48_tile_ss_col(percol ? (uint32_t)(i / percol) : 0u);
            if (py) *py = n48_tile_ss_nonrow(h, k);
            return 1;
        }
        i -= cols;
    }
    for (cy = 0; cy < N48_TILE_SS_GRID; cy++) {
        const uint32_t y = n48_tile_ss_grid_y(cy, h);
        if (n48_tile_ss_row_of(y)) continue;
        for (cx = 0; cx < N48_TILE_SS_GRID; cx++) {
            const uint32_t x = n48_tile_ss_grid_x(cx, w);
            if (n48_tile_ss_col_of(x)) continue;
            if (i == 0) { if (px) *px = x; if (py) *py = y; return 1; }
            i--;
        }
    }
    return 0;
}
// S7's decode: 0 = the poison (which no probe pixel can be on any surface this driver tests), else the value
// is a probe pixel and (x,y) are its low/high 16 bits.
#define N48_TILE_PROBE_POISON 0xDEADBEEFu
static inline int n48_tile_decode(uint32_t v, uint32_t *px, uint32_t *py)
{
    if (v == N48_TILE_PROBE_POISON) return 0;
    if (px) *px = v & 0xffffu;
    if (py) *py = v >> 16;
    return 1;
}

// Test client (tools/pc/m4-flush.c writes it into its surface): 16-pixel horizontal stripes alternating
// magenta and green, a 4-pixel white left border, with (x & 0xff) in the undisplayed byte.
static inline uint32_t n48_client_pixel(uint32_t x, uint32_t y)
{
    uint32_t c;
    if (x < 4u) c = 0x00ffffffu;
    else c = ((y / 16u) & 1u) ? 0x0000ff00u : 0x00ff00ffu;
    return c | ((x & 0xffu) << 24);
}

#ifdef __cplusplus
}
#endif

#endif // N48_SCANOUT_COPY_H
