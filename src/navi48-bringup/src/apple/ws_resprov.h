// ws_resprov.h -: OUR RESIDENCY COPY AS A PROVENANCE SOURCE for the descriptor path's producer
// ledger, and the gfx10 -> gfx12 RE-TILE the copy must do for it to be true. Pure C11 / C++17, no libc, no kernel types; host-tested
// by tests/ws_resprov_test.cpp and compiled unchanged by the kext (AppleHardwareHook.cpp, Navi48AccelPeer.cpp).
//
// WHY. F3 samples WindowServer's 28x40 window texture at VA 0x400006000 (gfx10 ADDR_SW_4KB_D_X, 32 bpp, 8 KB). The descriptor path
// refuses it on PROVENANCE because nothing ledgered its writer; hp5 showed the writer is OUR residency copy, a CPU copy of
// WindowServer's own resource from system memory into VRAM. That copy is a trusted path we control - but it copies bytes LAID OUT
// FOR gfx10, and gfx1201's texture unit reads gfx12 ADDR3_4KB_2D. The two layouts are NOT the same (every texel computed by
// the vendored addrlib, tools/addrlib-golden/):
//
//   address bit      2   3   4   5   6   7   8        9        10       11       12..
//   gfx12 4KB_2D     X0  Y0  X1  Y1  X2  Y2  Y3       X3       Y4       X4       block index (row-major, pitch in 32-texel blocks)
//   gfx10 4KB_D_X    X0  X1  Y0  Y1  X2  Y2  Y3^X6    X3^Y6    Y4^X5    X4^Y5    block index (the same)
//
// for a gfx10 GB_ADDR_CONFIG with PIPE_INTERLEAVE 256 B and 16 or 32 pipes (NUM_PIPES 4 or 5, NUM_PKRS within 2 of it) - the six
// configs 0x244 0x344 0x444 0x345 0x445 and 0x08200545 (this card's own register) give IDENTICAL hashes over seven surfaces; 1, 2, 4
// and 8 pipes do NOT (their XOR terms fall inside the block). Bits 3/4 transpose inside every 8x8 unit for every config; the XOR
// terms move whole 256 B units between rows/columns for surfaces taller or wider than 32. So a resource the copy moves byte for byte
// is SCRAMBLED for gfx12, and ledgering it as-is would vouch for a wrong picture.
//
// THE RULES (each a refusal-direction choice):
//   - RE-TILE only a resource whose own fields prove the shape: swizzle ADDR_SW_4KB_D_X in THE DWORD THE HARDWARE IS TOLD (:
//     setupHwCBRegs 0xbdf912c-0xbdf9145 reads *(res+0x180)+0x40 when res+0x180 is set, else res+0x1dc; [4:0] swizzle, [6:5] type,
//     0xbdf928d-0xbdf92f8), RESOURCE_TYPE = 1 (2D), depth 1, rowBytes
//     ... and NOT a second opinion from res+0x1dc: (hp7) measured res+0x1dc == 0x20 on all 1806 copies of the boot, so's
//     cross-check ("0 = never written, else it must agree in [6:0]") refused every texture including F3's. Apple's own `cmoveq
//     %rsi,%rcx` at 0xbdf9141 takes res+0x1dc ONLY when the load of res+0x180 set ZF, so with res+0x180 set the hardware never
//     reads res+0x1dc at all and a disagreement there changes nothing the hardware does. It is now a counted WARNING
//     (n48_rp_mask_warn), not a refusal. What is still cross-checked is what the hardware DOES honour, both out of that one
//     dword: [4:0] must be 22 (else the re-tile equations do not describe the bytes) and [6:5] must be 1, because setupHwCBRegs
//     programs the same dword's [6:5] into RESOURCE_TYPE (0xbdf928d shrl $0x5, andl $0x3 ... 0xbdf92f8 shll $0x18) and our
//     equations are the 2D, single-slice ones. Fail-closed everywhere else: res+0x180 NULL falls back to res+0x1dc and must
//     itself say 22 (it never does: 0x20); res+0x180 set but NOT a readable kernel record refuses MASK; any swizzle outside the
//     proven set refuses MODE.
//     = the ADDR_SW_4KB_D_X pitch at 32 bpp = 4 x align(width, 32) (: the vendored addrlib's pitch, ws_resprov_golden.h kG10),
//     width/height in 1..16384, and bytes == ceil(w/32) x ceil(h/32) x 4096 (one mip, one slice) - AND Apple's own gfx10
//     GB_ADDR_CONFIG, read through an exact-address-guarded path, in the proven class. Anything else keeps the plain copy and is NOT
//     ledgered (a tiled mode we cannot re-tile never enters the ledger).
//   - LEDGER only a copy that (a) returned success with every dword read back and 0 MISMATCHED, (b) ran in the process of the BOUND
//     WindowServer (ws_ident.h) and keys the bound context (our creation sequence number, live + unmap-hooked), (c) has a GPU VA from
//     the destination map, page aligned, below 2^48, (d) landed in ONE contiguous VRAM range, (e) carries a non-linear gfx12 mode.
//   - ANSWER yes (desc_tiled_ok) only for the SAME context, VA and gfx12 mode, under the SAME arm level and ledger epoch it was
//     recorded under (any untranslated pass, arm change or busy unmap moves the epoch), AND only if every 4 KiB page of [va, va+bytes)
//     resolves, through the asking frame's own VM, to exactly the VRAM range the copy wrote.
//   - CLEAR on the context's unmapVA, on WindowServer's drop/rebind (entries of any other context), and whole on any arm change.
//   - PAGE-OUT SYMMETRY: once anything was re-tiled this boot, a page-out (VRAM -> system memory) of a 4KB_D_X resource is REFUSED
//     (it keeps the skip), so system memory never receives gfx12-ordered bytes that a later page-in would re-tile a second time.
#ifndef N48_WS_RESPROV_H
#define N48_WS_RESPROV_H

#include <stdint.h>

#define N48_RP_G10_4KB_D_X   22u        /* AddrSwizzleMode ADDR_SW_4KB_D_X */
#define N48_RP_G12_4KB_2D    2u         /* Addr3SwizzleMode ADDR3_4KB_2D (xlat12_desc.h maps 22 -> 2 by name) */
#define N48_RP_MAX_DIM       16384u
#define N48_RP_MAX_BYTES     (1u << 20) /* the kext buffers two copies of the resource; 1 MiB each at most */

/* ---- 1. the two layouts, 4 bytes per element ---------------------------------------------------------------------------------- */
/* gfx10 GB_ADDR_CONFIG as addrlib's Gfx10Lib::HwlInitGlobalParams decodes it: NUM_PIPES [2:0], PIPE_INTERLEAVE_SIZE [5:3],
 * NUM_PKRS [10:8] (chip/gfx10/gfx10_gb_reg.h). 1 = the 4KB_D_X equation below is addrlib's for this config (golden-tested). */
static inline int n48_rp_g10_cfg_ok(uint32_t gb)
{
    const uint32_t np = gb & 7u, pi = (gb >> 3) & 7u, nk = (gb >> 8) & 7u;
    if (pi != 0u) return 0;                       /* addrlib asserts 256 B; nothing else was computed */
    if (np != 4u && np != 5u) return 0;           /* 16 or 32 pipes: the proven class */
    if (nk > np || np - nk > 2u) return 0;        /* addrlib's own RbPlus assertion */
    return 1;
}

#define N48_RP_B(v, i) (((v) >> (i)) & 1u)
/* Byte offset of texel (x, y) in a gfx10 ADDR_SW_4KB_D_X surface (proven config class), `pb` = pitch in 32-texel blocks. */
static inline uint32_t n48_rp_g10_off(uint32_t x, uint32_t y, uint32_t pb)
{
    const uint32_t in = (N48_RP_B(x, 0) << 2) | (N48_RP_B(x, 1) << 3) | (N48_RP_B(y, 0) << 4) | (N48_RP_B(y, 1) << 5) |
                        (N48_RP_B(x, 2) << 6) | (N48_RP_B(y, 2) << 7) |
                        ((N48_RP_B(y, 3) ^ N48_RP_B(x, 6)) << 8) | ((N48_RP_B(x, 3) ^ N48_RP_B(y, 6)) << 9) |
                        ((N48_RP_B(y, 4) ^ N48_RP_B(x, 5)) << 10) | ((N48_RP_B(x, 4) ^ N48_RP_B(y, 5)) << 11);
    return in + ((((y >> 5) * pb) + (x >> 5)) << 12);
}
/* Byte offset of texel (x, y) in a gfx12 ADDR3_4KB_2D surface (gfx12SwizzlePattern.h GFX12_SW_4KB_2D_1xAA 4-BPE row {2,3,0,0}). */
static inline uint32_t n48_rp_g12_off(uint32_t x, uint32_t y, uint32_t pb)
{
    const uint32_t in = (N48_RP_B(x, 0) << 2) | (N48_RP_B(y, 0) << 3) | (N48_RP_B(x, 1) << 4) | (N48_RP_B(y, 1) << 5) |
                        (N48_RP_B(x, 2) << 6) | (N48_RP_B(y, 2) << 7) | (N48_RP_B(y, 3) << 8) | (N48_RP_B(x, 3) << 9) |
                        (N48_RP_B(y, 4) << 10) | (N48_RP_B(x, 4) << 11);
    return in + ((((y >> 5) * pb) + (x >> 5)) << 12);
}

/* The allocation both layouts give a single-mip, single-slice w x h surface at 4 bytes per element. */
static inline uint64_t n48_rp_bytes(uint32_t w, uint32_t h)
{
    return (uint64_t)((w + 31u) / 32u) * (uint64_t)((h + 31u) / 32u) * 4096ull;
}

/* Move every dword of the allocation (padding included, so the map is a bijection over all of it). dir 1: gfx10 -> gfx12 (the
 * page-in re-tile); dir 0: gfx12 -> gfx10 (its inverse). src and dst must not overlap. Returns 1, or 0 on a bad argument
 * (nothing written). */
static inline int n48_rp_retile(const uint32_t *src, uint32_t *dst, uint32_t w, uint32_t h, uint64_t bytes, int dir)
{
    if (!src || !dst || !w || !h || w > N48_RP_MAX_DIM || h > N48_RP_MAX_DIM) return 0;
    if (bytes != n48_rp_bytes(w, h) || bytes > N48_RP_MAX_BYTES) return 0;
    const uint32_t pb = (w + 31u) / 32u, rows = (h + 31u) / 32u;
    for (uint32_t y = 0; y < rows * 32u; y++)
        for (uint32_t x = 0; x < pb * 32u; x++) {
            const uint32_t a = n48_rp_g10_off(x, y, pb) >> 2, b = n48_rp_g12_off(x, y, pb) >> 2;
            if (dir) dst[b] = src[a]; else dst[a] = src[b];
        }
    return 1;
}

/* ---- 1b. T450 = RESPROV PART 1: the census's other class-C (mode, bpp) pairs -------------------------------------------------
 * an internal design note Q3's "holes to fix first" 3 (256B_D needs its own re-tile), read together with hole 1's
 * fix above; item 1's census (notes/logs/runs/decide42/vmib-dumps/slot-table-A.txt, corrected by the reviewer:
 * P samples 0x400003000 (slot 10, gfx10 format 1, SW_MODE 2), 0x400032000 (slot 14, format 1, SW_MODE 2) and 0x40023c000
 * (slot 42, format 1, SW_MODE 22); AL samples 0x400025000 (slot 22, format 71, SW_MODE 22). xlat12_desc.h's
 * kXlat12FormatG10ToG12 names format 1 "8_UNORM" (one 8-bit channel = 1 byte/element = 8 bpp) and format 71
 * "16_16_16_16_FLOAT" (four 16-bit channels = 8 bytes/element = 64 bpp) - neither is the 32 bpp the equations
 * above were derived for, and SW_MODE 2 (ADDR_SW_256B_D) is not a mode those equations cover at any bpp. So ALL FOUR
 * class-C inputs the census names are refused by TODAY's re-tile (item 1's table in the T450 report has the detail).
 *
 * kXlat12SwModeG10ToG12 maps SW_MODE 2 (ADDR_SW_256B_D) -> gfx12 mode 1 (ADDR3_256B_2D) and SW_MODE 22 (ADDR_SW_4KB_D_X) ->
 * gfx12 mode 2 (ADDR3_4KB_2D, same as N48_RP_G12_4KB_2D above), by block size (addrlib: 256B_D's block is always 256 bytes,
 * 4KB_D_X's is always 4096 bytes, whatever the bpp - only the block's WIDTH x HEIGHT in elements changes).
 *
 * Every equation below is CONFIRMED against the vendored addrlib (tools/addrlib-golden/, ws_resprov_golden.h's
 * kG10_4kbdx8/kG12_4kbdx8, kG10_4kbdx64/kG12_4kbdx64, kG10_256d8/kG12_256d8), the SAME ten GB_ADDR_CONFIGs used:
 * all SIX proven-class configs (n48_rp_g10_cfg_ok) give IDENTICAL hashes for all three new (mode, bpp) pairs, exactly as
 * they do for 4KB_D_X at 32 bpp - n48_rp_g10_cfg_ok is reused UNCHANGED as the gate for all four kinds. Measured directly
 * (not golden-tested, since it is not one of the ten configs, but recorded here because it changes what "no XOR" means):
 * ADDR_SW_256B_D (no "_X") has NO GB_ADDR_CONFIG-dependent term at all in its equation - it matched a 1-pipe out-of-class
 * config too - while ADDR_SW_4KB_D_X's pipe/bank hash differs for out-of-class configs at 8 and 64 bpp exactly as it does
 * at 32 bpp. n48_rp_g10_cfg_ok is still applied to 256B_D anyway, fail-closed, since only ten configs were ever measured.
 *
 * THE DESIGN'S INFERENCE ("gfx12 256B_2D with the bit-3/4 transpose inside each 256 B block and no XOR terms") HELD, once
 * tested against surfaces whose PITCH is a power-of-two multiple of the block (addrlib's block-index term is an ordinary
 * integer multiply by the pitch-in-blocks, exactly like n48_rp_g10_off/n48_rp_g12_off above; a non-power-of-two pitch in
 * blocks, e.g. 1000 elements wide at 8 bpp = 63 blocks, makes that multiply look "nonlinear" under a naive per-bit GF(2)
 * probe, which is a property of the probe, not of the hardware - n48_rp_bytes-style padding to a whole block already
 * avoids ever needing a non-integer block count). 256B_D genuinely has NO pipe/bank XOR at any surface size (CONFIRMED,
 * matching addrlib's own SW_MODE naming: only the "_X" swizzles pipe/bank-interleave); 4KB_D_X at 8 and 64 bpp DOES have
 * one (CONFIRMED, same as the existing 32 bpp equation), with a different bit assignment per bpp because the "in" formula
 * is a BYTE offset and the element size (bppBits/8) changes which byte-bit an element-bit lands on. */
#define N48_RP_G10_256B_D    2u   /* AddrSwizzleMode ADDR_SW_256B_D */
#define N48_RP_G12_256B_2D   1u   /* Addr3SwizzleMode ADDR3_256B_2D (xlat12_desc.h maps 2 -> 1 by name) */

/* The four (gfx10 SW_MODE, bpp) pairs the goldens cover, or NONE for anything else - "a tiled mode we cannot re-tile
 * never enters the ledger". Kept as an explicit whitelist rather than inferring coverage from the mode alone,
 * because 4KB_D_X is covered at three bpps and 256B_D at only one so far (item 1's table). */
enum { N48_RP_KIND_4KB_D_X_32 = 0, N48_RP_KIND_4KB_D_X_8, N48_RP_KIND_4KB_D_X_64, N48_RP_KIND_256B_D_8, N48_RP_KIND_NONE };
static inline uint32_t n48_rp_kind(uint32_t g10Mode, uint32_t bpp)
{
    if (g10Mode == N48_RP_G10_4KB_D_X && bpp == 32u) return N48_RP_KIND_4KB_D_X_32;
    if (g10Mode == N48_RP_G10_4KB_D_X && bpp == 8u)  return N48_RP_KIND_4KB_D_X_8;
    if (g10Mode == N48_RP_G10_4KB_D_X && bpp == 64u) return N48_RP_KIND_4KB_D_X_64;
    if (g10Mode == N48_RP_G10_256B_D  && bpp == 8u)  return N48_RP_KIND_256B_D_8;
    return N48_RP_KIND_NONE;
}

/* gfx10 ADDR_SW_4KB_D_X at 8 bpp (block 64x64 elements = 4096 B; addrlib's pitch alignment measured at 64 elements).
 * Golden: kG10_4kbdx8. */
static inline uint32_t n48_rp_g10_off_4kbdx8(uint32_t x, uint32_t y, uint32_t pb)
{
    const uint32_t in = (N48_RP_B(x, 0) << 0) | (N48_RP_B(x, 1) << 1) | (N48_RP_B(x, 2) << 2) | (N48_RP_B(y, 1) << 3) |
                        (N48_RP_B(y, 0) << 4) | (N48_RP_B(y, 2) << 5) | (N48_RP_B(x, 3) << 6) | (N48_RP_B(y, 3) << 7) |
                        ((N48_RP_B(x, 7) ^ N48_RP_B(y, 4)) << 8) | ((N48_RP_B(x, 4) ^ N48_RP_B(y, 7)) << 9) |
                        ((N48_RP_B(x, 6) ^ N48_RP_B(y, 5)) << 10) | ((N48_RP_B(x, 5) ^ N48_RP_B(y, 6)) << 11);
    return in + ((((y >> 6) * pb) + (x >> 6)) << 12);
}
/* gfx12 ADDR3_4KB_2D at 8 bpp (same block; gfx12 has no pipe/bank hash at any bpp). Golden: kG12_4kbdx8. */
static inline uint32_t n48_rp_g12_off_4kbdx8(uint32_t x, uint32_t y, uint32_t pb)
{
    const uint32_t in = (N48_RP_B(x, 0) << 0) | (N48_RP_B(x, 1) << 1) | (N48_RP_B(y, 0) << 2) | (N48_RP_B(x, 2) << 3) |
                        (N48_RP_B(y, 1) << 4) | (N48_RP_B(y, 2) << 5) | (N48_RP_B(x, 3) << 6) | (N48_RP_B(y, 3) << 7) |
                        (N48_RP_B(y, 4) << 8) | (N48_RP_B(x, 4) << 9) | (N48_RP_B(y, 5) << 10) | (N48_RP_B(x, 5) << 11);
    return in + ((((y >> 6) * pb) + (x >> 6)) << 12);
}
/* gfx10 ADDR_SW_4KB_D_X at 64 bpp (block 32(w) x 16(h) elements = 4096 B - NOT square, since 4096 B / 8 B per element =
 * 512 elements; addrlib's pitch alignment measured at 32 elements, block-height crossing measured at y=16). Golden:
 * kG10_4kbdx64. */
static inline uint32_t n48_rp_g10_off_4kbdx64(uint32_t x, uint32_t y, uint32_t pb)
{
    const uint32_t in = (N48_RP_B(x, 0) << 3) | (N48_RP_B(y, 0) << 4) | (N48_RP_B(x, 1) << 5) | (N48_RP_B(x, 2) << 6) |
                        (N48_RP_B(y, 1) << 7) | ((N48_RP_B(x, 6) ^ N48_RP_B(y, 2)) << 8) |
                        ((N48_RP_B(x, 3) ^ N48_RP_B(y, 5)) << 9) | ((N48_RP_B(x, 5) ^ N48_RP_B(y, 3)) << 10) |
                        ((N48_RP_B(x, 4) ^ N48_RP_B(y, 4)) << 11);
    return in + ((((y >> 4) * pb) + (x >> 5)) << 12);
}
/* gfx12 ADDR3_4KB_2D at 64 bpp (same block, no XOR). Golden: kG12_4kbdx64. */
static inline uint32_t n48_rp_g12_off_4kbdx64(uint32_t x, uint32_t y, uint32_t pb)
{
    const uint32_t in = (N48_RP_B(x, 0) << 3) | (N48_RP_B(y, 0) << 4) | (N48_RP_B(x, 1) << 5) | (N48_RP_B(x, 2) << 6) |
                        (N48_RP_B(y, 1) << 7) | (N48_RP_B(y, 2) << 8) | (N48_RP_B(x, 3) << 9) | (N48_RP_B(y, 3) << 10) |
                        (N48_RP_B(x, 4) << 11);
    return in + ((((y >> 4) * pb) + (x >> 5)) << 12);
}
/* gfx10 ADDR_SW_256B_D at 8 bpp (block 16x16 elements = 256 B; addrlib's pitch alignment measured at 16 elements). NO
 * pipe/bank XOR (measured config-independent, see the section banner above). Golden: kG10_256d8. */
static inline uint32_t n48_rp_g10_off_256d8(uint32_t x, uint32_t y, uint32_t pb)
{
    const uint32_t in = (N48_RP_B(x, 0) << 0) | (N48_RP_B(x, 1) << 1) | (N48_RP_B(x, 2) << 2) | (N48_RP_B(y, 1) << 3) |
                        (N48_RP_B(y, 0) << 4) | (N48_RP_B(y, 2) << 5) | (N48_RP_B(x, 3) << 6) | (N48_RP_B(y, 3) << 7);
    return in + ((((y >> 4) * pb) + (x >> 4)) << 8);
}
/* gfx12 ADDR3_256B_2D at 8 bpp. Golden: kG12_256d8. */
static inline uint32_t n48_rp_g12_off_256d8(uint32_t x, uint32_t y, uint32_t pb)
{
    const uint32_t in = (N48_RP_B(x, 0) << 0) | (N48_RP_B(x, 1) << 1) | (N48_RP_B(y, 0) << 2) | (N48_RP_B(x, 2) << 3) |
                        (N48_RP_B(y, 1) << 4) | (N48_RP_B(y, 2) << 5) | (N48_RP_B(x, 3) << 6) | (N48_RP_B(y, 3) << 7);
    return in + ((((y >> 4) * pb) + (x >> 4)) << 8);
}

/* Block geometry and the allocation size for one of the four golden-tested kinds, or 0 for N48_RP_KIND_NONE. Mirrors
 * n48_rp_bytes's rule (padding included, so the whole allocation is covered) generalised over block shape and element
 * size; reduces to n48_rp_bytes(w, h) exactly for N48_RP_KIND_4KB_D_X_32. */
static inline void n48_rp_kind_geom(uint32_t kind, uint32_t *blockW, uint32_t *blockH, uint32_t *elemBytes)
{
    switch (kind) {
    case N48_RP_KIND_4KB_D_X_32: *blockW = 32u; *blockH = 32u; *elemBytes = 4u; break;
    case N48_RP_KIND_4KB_D_X_8:  *blockW = 64u; *blockH = 64u; *elemBytes = 1u; break;
    case N48_RP_KIND_4KB_D_X_64: *blockW = 32u; *blockH = 16u; *elemBytes = 8u; break;
    case N48_RP_KIND_256B_D_8:   *blockW = 16u; *blockH = 16u; *elemBytes = 1u; break;
    default:                     *blockW = 0u;  *blockH = 0u;  *elemBytes = 0u; break;
    }
}
static inline uint64_t n48_rp_kind_bytes(uint32_t kind, uint32_t w, uint32_t h)
{
    uint32_t bw, bh, eb; n48_rp_kind_geom(kind, &bw, &bh, &eb);
    if (!bw || !w || !h || w > N48_RP_MAX_DIM || h > N48_RP_MAX_DIM) return 0ull;
    return (uint64_t)((w + bw - 1u) / bw) * (uint64_t)((h + bh - 1u) / bh) * (uint64_t)bw * (uint64_t)bh * (uint64_t)eb;
}
/* The pitch in BYTES addrlib reports for this kind (element pitch, aligned up to a whole block, times the element size) -
 * the generalisation of n48_rp_pitch_bytes over kind; reduces to it exactly for N48_RP_KIND_4KB_D_X_32. */
static inline uint32_t n48_rp_kind_pitch_bytes(uint32_t kind, uint32_t w)
{
    uint32_t bw, bh, eb; n48_rp_kind_geom(kind, &bw, &bh, &eb); (void)bh;
    if (!bw || !w || w > N48_RP_MAX_DIM) return 0u;
    return eb * ((w + bw - 1u) & ~(bw - 1u));
}

/* Move every element of the allocation for one of the four golden-tested kinds (dir 1: gfx10 -> gfx12 page-in re-tile;
 * dir 0: its inverse - the same page-out-symmetry pairing n48_rp_retile provides for the wired 32 bpp case). Buffers are
 * raw bytes because the element width differs by kind (1, 4 or 8 bytes); n48_rp_retile above is UNCHANGED and stays the
 * kext's one wired entry point (Navi48AccelPeer.cpp) for N48_RP_KIND_4KB_D_X_32 - this is a SIBLING for the kext to wire
 * to later (T450's brief: "do not wire it"), covering the other three kinds plus that one for a caller that already
 * knows its kind. Returns 1, or 0 on a bad argument OR N48_RP_KIND_NONE (nothing written either way - the goldens do not
 * cover it, so it must keep the plain copy and stay out of the ledger, exactly as requires). src and dst must not
 * overlap. */
static inline int n48_rp_retile_kind(uint32_t kind, const uint8_t *src, uint8_t *dst, uint32_t w, uint32_t h, uint64_t bytes,
                                     int dir)
{
    if (!src || !dst || !w || !h || w > N48_RP_MAX_DIM || h > N48_RP_MAX_DIM || kind >= N48_RP_KIND_NONE) return 0;
    uint32_t bw, bh, eb; n48_rp_kind_geom(kind, &bw, &bh, &eb);
    if (bytes != n48_rp_kind_bytes(kind, w, h) || bytes > N48_RP_MAX_BYTES) return 0;
    const uint32_t pb = (w + bw - 1u) / bw, rows = (h + bh - 1u) / bh;
    for (uint32_t y = 0; y < rows * bh; y++)
        for (uint32_t x = 0; x < pb * bw; x++) {
            uint32_t a, b;
            switch (kind) {
            case N48_RP_KIND_4KB_D_X_32: a = n48_rp_g10_off(x, y, pb);        b = n48_rp_g12_off(x, y, pb);        break;
            case N48_RP_KIND_4KB_D_X_8:  a = n48_rp_g10_off_4kbdx8(x, y, pb); b = n48_rp_g12_off_4kbdx8(x, y, pb); break;
            case N48_RP_KIND_4KB_D_X_64: a = n48_rp_g10_off_4kbdx64(x, y, pb);b = n48_rp_g12_off_4kbdx64(x, y, pb);break;
            default:                     a = n48_rp_g10_off_256d8(x, y, pb); b = n48_rp_g12_off_256d8(x, y, pb);  break;
            }
            const uint8_t *s = dir ? src + a : src + b;
            uint8_t *d = dir ? dst + b : dst + a;
            for (uint32_t k = 0; k < eb; k++) d[k] = s[k];
        }
    return 1;
}

/* ---- 2. the shape proof ------------------------------------------------------------------------------------------------------- */
enum {
    N48_RP_SHAPE_OK = 0, N48_RP_SHAPE_MODE = 1, N48_RP_SHAPE_MASK = 2, N48_RP_SHAPE_TYPE = 3, N48_RP_SHAPE_BPE = 4,
    N48_RP_SHAPE_DEPTH = 5, N48_RP_SHAPE_DIMS = 6, N48_RP_SHAPE_BYTES = 7, N48_RP_SHAPE_CFG_UNREAD = 8, N48_RP_SHAPE_CFG_CLASS = 9,
    /* build 0.0.451 item 1 (S1, review of 0.0.450): Apple's own allocator rounds a resource's byte count UP TO A
     * WHOLE PAGE (0x40023c000: 209x41 at 8 bpp in 4KB_D_X needs 0x4000 by the block layout - already page-sized, so no
     * rounding helps it - and Apple allocated only 0x3000, LESS than the layout: re-tiling it would read/write past a
     * 12 KiB allocation as if it were 16 KiB. That is not a page-rounding case at all - it is UNDERSIZED, and re-tiling
     * it would be unsafe (an out-of-bounds walk over the copy's own buffers). This reason names exactly that: `bytes`
     * is less than the kind's own exact layout size, whatever the page rounding). N48_RP_SHAPE_BYTES (7, unchanged)
     * still covers every OTHER byte-count mismatch (too large, or an odd non-page value) - fail-closed as before. */
    N48_RP_SHAPE_BYTES_SMALL = 10,
    N48_RP_SHAPE_REASONS = 11
};
/* build 0.0.451 item 1: Apple's allocator rounds UP TO A WHOLE 4 KiB PAGE (measured: P's 0x400003000/0x400032000,
 *.c - a 256B_D layout of 0x800/0x1200 bytes lands in a 0x1000/0x2000 allocation, exactly the next page).
 * 0 on overflow past N48_RP_MAX_BYTES's own range (never reached in practice: layouts here are far under 1 MiB). */
static inline uint64_t n48_rp_round4096(uint64_t bytes)
{
    return (bytes + 4095ull) & ~(uint64_t)4095u;
}
typedef struct {
    uint32_t surf;       /* res+0x1dc (AMDAccelResourceAddr2::shapeSurfaceBuffer's record: [4:0] swizzle, [6:5] resource type) */
    uint32_t hasMask;    /* res+0x180 non-null: the per-resource surface record initialize copies from the create args */
    uint32_t maskOk;     /*: 1 = that pointer is a readable kernel record and maskSurf came out of it. hasMask && !maskOk
                          * is the fail-closed case (N48_RP_SHAPE_MASK): the hardware WOULD read a dword we could not. */
    uint32_t maskSurf;   /* *(res+0x180)+0x40 when hasMask && maskOk */
    uint32_t w, h, depth, rowBytes;   /* res+0xb0 / +0xb2 (u16), +0xb4, +0xb8 */
    uint64_t bytes;      /* res+0x230 */
    uint32_t gbRead;     /* 1 = Apple's gfx10 GB_ADDR_CONFIG was read through the guarded path */
    uint32_t gb;
} n48_rp_shape;

/*: the dword setupHwCBRegs hands the hardware (0xbdf9141 cmoveq): *(res+0x180)+0x40 when res+0x180 is set, else res+0x1dc. */
static inline uint32_t n48_rp_swz_dword(const n48_rp_shape *s)
{
    return s->hasMask ? s->maskSurf : s->surf;
}

/*: what res+0x1dc says when the hardware is NOT reading it. A disagreement is a NOTE, because Apple's `cmoveq %rsi,%rcx`
 * (0xbdf9141; %rcx = *(res+0x180)+0x40, %rsi = res+0x1dc) keeps %rcx whenever res+0x180 is non-zero - so nothing the hardware
 * does depends on res+0x1dc for such a resource. hp7's census: res+0x1dc == 0x20 on ALL 1806 copies (mode 0, type 1), while the
 * record said 0x36 for F3's texture; refusing on that is stricter than the hardware and refused every texture. Bit 0 = the
 * swizzle bits [4:0] differ, bit 1 = the type bits [6:5] differ. 0 = nothing to warn about (or no record at all). */
enum { N48_RP_WARN_SWZ = 1u, N48_RP_WARN_TYPE = 2u };
static inline uint32_t n48_rp_mask_warn(const n48_rp_shape *s)
{
    uint32_t w = 0u;
    if (!s || !s->hasMask || s->maskOk != 1u) return 0u;
    if ((s->surf & 0x1fu) != (s->maskSurf & 0x1fu)) w |= N48_RP_WARN_SWZ;
    if (((s->surf >> 5) & 3u) != ((s->maskSurf >> 5) & 3u)) w |= N48_RP_WARN_TYPE;
    return w;
}
/*: the ADDR_SW_4KB_D_X pitch in bytes at 32 bpp - addrlib's Addr2ComputeSurfaceInfo pitch, align(width, 32) elements, for
 * every gfx10 config in ws_resprov_golden.h's kG10 (28 -> 32, 100 -> 128, 1000 -> 1024, 33 -> 64, 1 -> 32). 0 on overflow. */
static inline uint32_t n48_rp_pitch_bytes(uint32_t w)
{
    if (!w || w > N48_RP_MAX_DIM) return 0u;
    return 4u * ((w + 31u) & ~31u);
}

static inline uint32_t n48_rp_shape_check(const n48_rp_shape *s)
{
    if (!s) return N48_RP_SHAPE_MODE;
    /*: the ONLY record that decides anything is the one the hardware reads. A res+0x180 we could not read is still a
     * refusal (the hardware would read a dword we never saw); a res+0x1dc that disagrees is only n48_rp_mask_warn. */
    if (s->hasMask && s->maskOk != 1u) return N48_RP_SHAPE_MASK;
    const uint32_t d = n48_rp_swz_dword(s);
    if ((d & 0x1fu) != N48_RP_G10_4KB_D_X) return N48_RP_SHAPE_MODE;   /* the swizzle the hardware is told: COLOR_SW_MODE */
    if (((d >> 5) & 3u) != 1u) return N48_RP_SHAPE_TYPE;               /* [6:5] of the SAME dword: RESOURCE_TYPE, must be 2D */
    if (!s->w || !s->h || s->w > N48_RP_MAX_DIM || s->h > N48_RP_MAX_DIM) return N48_RP_SHAPE_DIMS;
    if (s->rowBytes != n48_rp_pitch_bytes(s->w)) return N48_RP_SHAPE_BPE;
    if (s->depth != 1u) return N48_RP_SHAPE_DEPTH;
    {   /* build 0.0.451 item 1 (S1): admit Apple's own page-rounded allocation too, for consistency with
         * n48_rp_shape_check_kind below - the exact layout size, or that size rounded up to the next 4 KiB page. */
        const uint64_t exact = n48_rp_bytes(s->w, s->h);
        if (s->bytes < exact) return N48_RP_SHAPE_BYTES_SMALL;
        if ((s->bytes != exact && s->bytes != n48_rp_round4096(exact)) || s->bytes > N48_RP_MAX_BYTES) return N48_RP_SHAPE_BYTES;
    }
    if (s->gbRead != 1u) return N48_RP_SHAPE_CFG_UNREAD;
    if (!n48_rp_g10_cfg_ok(s->gb)) return N48_RP_SHAPE_CFG_CLASS;
    return N48_RP_SHAPE_OK;
}

/* build 0.0.450 item 1 (switch 46, DEFAULT OFF, AppleHardwareHook.cpp / Navi48AccelPeer.cpp): generalises
 * n48_rp_shape_check ABOVE over one of the four golden-tested kinds. Reduces to it EXACTLY for
 * N48_RP_KIND_4KB_D_X_32 (same fields, same order, same reasons, same n48_rp_pitch_bytes/n48_rp_bytes formulas via
 * n48_rp_kind_pitch_bytes/n48_rp_kind_bytes reducing to them for that kind) - n48_rp_shape_check ITSELF IS
 * UNCHANGED, so the switch-OFF path keeps calling it, unmodified, and is byte-identical to 0.0.449. The only
 * behavioural difference from n48_rp_shape_check is which G10 mode is demanded ([4:0] of the swizzle dword): 22
 * (4KB_D_X) for the three 4KB_D_X kinds, 2 (256B_D) for N48_RP_KIND_256B_D_8. n48_rp_g10_cfg_ok is unchanged and
 * applied to every kind, fail-closed, exactly as the banner above n48_rp_kind requires even for 256B_D (which has
 * no config-dependent term, but was only measured on the ten proven configs). */
static inline uint32_t n48_rp_shape_check_kind(const n48_rp_shape *s, uint32_t kind)
{
    if (!s || kind >= N48_RP_KIND_NONE) return N48_RP_SHAPE_MODE;
    uint32_t bw, bh, eb; n48_rp_kind_geom(kind, &bw, &bh, &eb);
    if (!bw) return N48_RP_SHAPE_MODE;
    const uint32_t wantMode = (kind == N48_RP_KIND_256B_D_8) ? N48_RP_G10_256B_D : N48_RP_G10_4KB_D_X;
    if (s->hasMask && s->maskOk != 1u) return N48_RP_SHAPE_MASK;
    const uint32_t d = n48_rp_swz_dword(s);
    if ((d & 0x1fu) != wantMode) return N48_RP_SHAPE_MODE;
    if (((d >> 5) & 3u) != 1u) return N48_RP_SHAPE_TYPE;
    if (!s->w || !s->h || s->w > N48_RP_MAX_DIM || s->h > N48_RP_MAX_DIM) return N48_RP_SHAPE_DIMS;
    if (s->rowBytes != n48_rp_kind_pitch_bytes(kind, s->w)) return N48_RP_SHAPE_BPE;
    if (s->depth != 1u) return N48_RP_SHAPE_DEPTH;
    {   /* build 0.0.451 item 1 (S1): admit `bytes` == the kind's exact layout size, OR that size rounded up to
         * the next 4 KiB page (Apple's own allocator does this - shapes.c: P's 0x400003000/0x400032000). Refuse a
         * SMALLER allocation by name (N48_RP_SHAPE_BYTES_SMALL): re-tiling it would read/write past the buffer the
         * copy actually allocated (0x40023c000: needs 0x4000, Apple gave 0x3000). Anything else (larger than the
         * rounded size, or an odd non-page value) is the ordinary N48_RP_SHAPE_BYTES mismatch, unchanged. */
        const uint64_t exact = n48_rp_kind_bytes(kind, s->w, s->h);
        if (s->bytes < exact) return N48_RP_SHAPE_BYTES_SMALL;
        if ((s->bytes != exact && s->bytes != n48_rp_round4096(exact)) || s->bytes > N48_RP_MAX_BYTES) return N48_RP_SHAPE_BYTES;
    }
    if (s->gbRead != 1u) return N48_RP_SHAPE_CFG_UNREAD;
    if (!n48_rp_g10_cfg_ok(s->gb)) return N48_RP_SHAPE_CFG_CLASS;
    return N48_RP_SHAPE_OK;
}

/* ---- 3. the provenance table -------------------------------------------------------------------------------------------------- */
/* build 0.0.552 ( PLAN (3); switch 109, gfx_rp109.h): N48_RP_MAX is the table's STORAGE (was 32, which was also its
 * capacity). The CAPACITY a record may fill is now chosen per call (n48_rp_record_x): N48_RP_CAP_OFF (32, switch 109 OFF / SHADOW:
 * 0.0.551's table exactly - the 33rd distinct entry is refused FULL) or N48_RP_CAP_ON (128, switch 109 ON). Every loop over the
 * table runs to t->n (bounded by N48_RP_MAX), so the storage alone changes no answer. 128 x sizeof(n48_rp_ent) (64 B) = 8 KiB in
 * the kext's one static table (gXdRp, AppleHardwareHook.cpp: __DATA, never on a stack); the host tests' stack copies grow by 6 KiB. */
#define N48_RP_MAX 128u
#define N48_RP_CAP_OFF 32u
#define N48_RP_CAP_ON 128u
enum {
    N48_RP_REC_OK = 0, N48_RP_REC_NOT_COPIED = 1, N48_RP_REC_UNVERIFIED = 2, N48_RP_REC_NOT_WS = 3, N48_RP_REC_NO_CTX = 4,
    N48_RP_REC_NO_VA = 5, N48_RP_REC_SPLIT = 6, N48_RP_REC_MODE = 7, N48_RP_REC_NOT_RETILED = 8, N48_RP_REC_FULL = 9,
    N48_RP_REC_REASONS = 10
};
/* build 0.0.451 item 2 (S4, review of 0.0.450): `elemBytes` (was `pad`, same field, same struct size) - the
 * kind's own bytes-per-element, stamped on every entry at record time (the 32 bpp path stores 4; the T450 kinds
 * store 1/8/1 for 8bpp/64bpp/256B_D@8bpp - ws_resprov.h n48_rp_kind_geom's own `eb`). Without this, the pitch-only
 * shape check admits a COLLISION: a 128 bpp 4KB_D_X resource with w <= 16 also satisfies the 64 bpp kind's pitch
 * formula, and a 16 bpp resource with w <= 32 also satisfies the 32 bpp kind's - so two DIFFERENT real resources
 * could land on the SAME (mode, VA) key with DIFFERENT element sizes, and the ask (n48_rp_ok/n48_rp_ok_carry) would
 * answer yes for either regardless of which one a LATER T# actually is. Compared at the ask against the T#'s own
 * declared element size (from its gfx10 format, xlat12_desc.h's xlat12_format_elem_bytes) - a mismatch refuses. */
/* build 0.0.486 (switch 59, an internal design note Q3/Q6): `lin`, `w`, `h` - 1 = this entry is a BACKING-SOURCED
 * copy (section 6 below: mip 0 of a LINEAR system-memory backing written as gfx12 ADDR3_4KB_2D - since 0.0.492 in the gfx12
 * mode of its VRAM side, `mode` 1, 2 or 3, bytes = the gfx12 length), with the image's own width
 * and height. Such an entry answers ONLY the T#-aware ask n48_rp_ok_t, and only for a T# that matches it (n48_rp_lin_t_match);
 * n48_rp_ok / n48_rp_ok_carry REFUSE it (linNoT), because they cannot see the T# whose MAX_MIP / LAST_LEVEL must be clamped.
 * 0 on every entry any other path records - zero-initialised copies keep today's entries exactly. */
/* build 0.0.493 (switch 59): `known` (was `pad2`, same place, same struct size) - 0 = not a known asset; k + 1 = the entry is
 * a KNOWN-ASSET conversion of row k of kN48RpKnownNib4 (section 7). Such an entry is also `lin` 1 (it answers only the T#-aware
 * ask, is refused by n48_rp_ok / _carry, and is dropped by a committed colour target like every lin entry), but its T# must match
 * the ROW (n48_rp_known_t_match: the row's gfx10 SW_MODE and FORMAT too), not n48_rp_lin_t_match. 0 on every other entry. */
typedef struct { uint64_t ctx, va, vram, bytes; uint32_t mode, arm, epoch, elemBytes; uint32_t lin, w, h, known; } n48_rp_ent;
typedef struct {
    uint32_t n, pad;
    n48_rp_ent e[N48_RP_MAX];
    uint64_t recorded, refused[N48_RP_REC_REASONS], asked, proven, walkRefused, stale, unmaps, sweeps, dropped;
    /* build 0.0.523 (RING-NEUTER-FORGIVE.md item 12(c),  (4)): `sweeps` was `rebinds` - n48_rp_rebind runs on
     * every locked hw_resprov_note_copy, so it counts SWEEPS, not binding changes (WindowServer's binding counters are
     * gWs.binds/rebinds, a different thing). `sweepDropped`: entries those sweeps dropped. APPENDED. */
    uint64_t sweepDropped;
    uint64_t elemMismatch;   /* build 0.0.451 item 2: asked with a DIFFERENT element size than the one recorded */
    /* build 0.0.486 (switch 59): linNoT = a lin entry asked WITHOUT the T# (n48_rp_ok / _carry): refused;
     * linTRefused = a lin entry asked WITH a T# that does not match it: refused; linProven = lin asks proven (each one hands
     * the caller the mip clamp); linLedDropped = lin entries dropped because a committed colour target overlaps them;
     * linLedNoExtent = committed targets with no derived extent that begin BELOW a lin entry (not dropped: an extent-less
     * target is taken as its base page only - notes the SUSPECTED residual, never a decision). */
    uint64_t linNoT, linTRefused, linProven, linLedDropped, linLedNoExtent;
    /* build 0.0.493: knownTRefused = a known-asset entry asked with a T# that does not match its row (also counted in
     * linTRefused); knownProven = known-asset asks proven (also counted in linProven). */
    uint64_t knownTRefused, knownProven;
    /* build 0.0.552 (switch 109): entries EVICTED to make room under ON (each an epoch-stale entry: n48_rp_evict_pick), and
     * FULL refusals under OFF / SHADOW that ON would have recorded (n48_rp_on_would_fit). APPENDED. */
    uint64_t evicted, fullOnWould;
} n48_rp;

/* One copy, as the kext has it after residency_copy_to_vram returned. Every field defaults to "unproven" when zeroed. */
typedef struct {
    uint32_t copied;              /* 1 = the copy returned success */
    uint32_t retiled;             /* 1 = the bytes written were n48_rp_retile's gfx12 image (0 = a byte-for-byte copy) */
    uint64_t bytes, compared, mismatched;   /* compared = dwords read back, mismatched = of those, different */
    uint32_t ownerWs;             /* 1 = the copying process is the BOUND WindowServer's */
    uint32_t lin;                 /* build 0.0.486 (switch 59): a backing-sourced copy (in what was padding: no size change).
                                   * build 0.0.493: N48_RP_LIN_KNOWN0 + k = a KNOWN-ASSET conversion of row k (section 7) */
    uint64_t ctx;                 /* the bound context's key (0 = none) */
    uint32_t vaOk;                /* 1 = the destination map's GPU VA was read (IOAccelMemoryMap non-physical branch) */
    uint32_t w;                   /* build 0.0.486: its image's width (padding before) */
    uint64_t va;
    uint32_t contiguous;          /* 1 = one VRAM range [vram, vram + bytes) */
    uint32_t h;                   /* build 0.0.486: its image's height (padding before) */
    uint64_t vram;
    uint32_t mode;                /* gfx12 mode the bytes are now in */
    uint32_t arm, epoch;          /* the ledger's arm level and epoch at record time */
    uint32_t elemBytes;           /* build 0.0.451 item 2: the re-tile kind's own bytes-per-element */
} n48_rp_copy;

static inline void n48_rp_drop_at(n48_rp *t, uint32_t k)
{
    for (uint32_t j = k; j + 1u < t->n && j + 1u < N48_RP_MAX; j++) {
        t->e[j].ctx = t->e[j + 1u].ctx; t->e[j].va = t->e[j + 1u].va; t->e[j].vram = t->e[j + 1u].vram;
        t->e[j].bytes = t->e[j + 1u].bytes; t->e[j].mode = t->e[j + 1u].mode; t->e[j].arm = t->e[j + 1u].arm;
        t->e[j].epoch = t->e[j + 1u].epoch; t->e[j].elemBytes = t->e[j + 1u].elemBytes;
        t->e[j].lin = t->e[j + 1u].lin; t->e[j].w = t->e[j + 1u].w; t->e[j].h = t->e[j + 1u].h;   /* build 0.0.486 */
        t->e[j].known = t->e[j + 1u].known;                                                         /* build 0.0.493 */
    }
    t->n--;
    t->dropped++;
}

static inline uint32_t n48_rp_lin_g10_for_g12(uint32_t g12);   /* build 0.0.492: section 6b (kN48RpLinModes) */
#define N48_RP_LIN_KNOWN0 2u                                     /* build 0.0.493: n48_rp_copy.lin of row 0 of section 7 */
static inline int n48_rp_known_copy_ok(const n48_rp_copy *c);   /* build 0.0.493: section 7 */
static inline int n48_rp_known_t_match(const n48_rp_ent *e, const uint32_t *t10);   /* build 0.0.493: section 7 */
/* build 0.0.552 (switch 109) — SUPERSEDED-ENTRY EVICTION. An entry recorded under ledger epoch E answers yes only at an ask
 * made under the SAME epoch (n48_rp_ok / _carry / _t: `e->epoch != epoch` -> stale, return 0 - before the walk); the kext's epoch
 * (gXdDpEpoch) only ever moves UP by one (every writer is an __atomic_fetch_add of 1), so once the epoch has moved past E the entry
 * can NEVER answer yes again: it only holds a slot. That is the one entry this picks: the OLDEST (lowest index: the table is
 * append-ordered) whose epoch is not `curEpoch`, or t->n when there is none. Evicting it turns a no into a no (not found); nothing
 * that could answer yes is ever evicted, and no entry is ever MADE: a record still needs every rule of n48_rp_record_x. Pure. */
static inline uint32_t n48_rp_evict_pick(const n48_rp *t, uint32_t curEpoch)
{
    for (uint32_t k = 0; k < t->n && k < N48_RP_MAX; k++) if (t->e[k].epoch != curEpoch) return k;
    return t->n;
}
/* 1 = a record that finds the table at its OFF capacity would fit under ON (room below N48_RP_CAP_ON, or an evictable entry). */
static inline uint32_t n48_rp_on_would_fit(const n48_rp *t, uint32_t curEpoch)
{
    return (t->n < N48_RP_CAP_ON || n48_rp_evict_pick(t, curEpoch) < t->n) ? 1u : 0u;
}
/* The table's state for switch 109's FULL line: entries whose epoch is not `curEpoch` (can never answer again), whose arm level
 * is not `curArm` (answer nothing at this arm level, but may again), and of a context other than `ctx`. Pure, read-only. */
static inline void n48_rp_census(const n48_rp *t, uint32_t curEpoch, uint32_t curArm, uint64_t ctx, uint32_t *epochStale,
                                 uint32_t *armStale, uint32_t *otherCtx)
{
    uint32_t a = 0u, b = 0u, o = 0u;
    for (uint32_t k = 0; k < t->n && k < N48_RP_MAX; k++) {
        if (t->e[k].epoch != curEpoch) a++;
        if (t->e[k].arm != curArm) b++;
        if (t->e[k].ctx != ctx) o++;
    }
    if (epochStale) *epochStale = a;
    if (armStale) *armStale = b;
    if (otherCtx) *otherCtx = o;
}
/* build 0.0.552 (switch 109): n48_rp_record with the capacity chosen by `on` (1 = switch 109 ON: N48_RP_CAP_ON and, when the
 * table is at it, one epoch-stale entry evicted first; 2 = SHADOW: 0.0.551's record exactly, and a FULL refusal ON would have
 * recorded is counted in fullOnWould; anything else (0 = OFF) = N48_RP_CAP_OFF, no eviction - 0.0.551's record exactly).
 * `curEpoch` is the ledger's epoch NOW (gXdDpEpoch under gXdLock), read only under ON. EVERY ADMISSION RULE IS UNCHANGED AND COMES
 * FIRST: the eviction is reached only by a copy that passed every one of them (`why` 0), after the superseding drop. */
static inline uint32_t n48_rp_record_x(n48_rp *t, const n48_rp_copy *c, uint32_t on, uint32_t curEpoch)
{
    uint32_t why = N48_RP_REC_OK;
    if (!c || c->copied != 1u) why = N48_RP_REC_NOT_COPIED;
    else if (!c->bytes || c->mismatched || c->compared * 4ull < c->bytes) why = N48_RP_REC_UNVERIFIED;
    else if (c->ownerWs != 1u) why = N48_RP_REC_NOT_WS;
    else if (!c->ctx) why = N48_RP_REC_NO_CTX;
    else if (c->vaOk != 1u || !c->va || (c->va & 0xfffull) || c->va >= (1ull << 48) || c->va + c->bytes > (1ull << 48))
        why = N48_RP_REC_NO_VA;
    else if (c->contiguous != 1u || (c->vram & 0xfffull)) why = N48_RP_REC_SPLIT;
    else if (!c->mode || c->mode > 7u) why = N48_RP_REC_MODE;
    else if (c->mode == N48_RP_G12_4KB_2D && c->retiled != 1u) why = N48_RP_REC_NOT_RETILED;
    /* build 0.0.486: a backing-sourced copy is only ever written as gfx12 ADDR3_4KB_2D and must name its image's
     * dimensions (the ask matches the T# against them). c->lin 0 (every other path) never reaches this clause. */
    /* build 0.0.492: ...or, since 0.0.492, in the gfx12 mode of any VRAM mode section 6 writes (kN48RpLinModes: 1, 2, 3),
     * always re-tiled. */
    else if (c->lin && (!n48_rp_lin_g10_for_g12(c->mode) || c->retiled != 1u || !c->w || !c->h)) why = N48_RP_REC_MODE;
    /* build 0.0.493: a known-asset conversion must be exactly its row's (mode, dims, element size, length). */
    else if (c->lin >= N48_RP_LIN_KNOWN0 && !n48_rp_known_copy_ok(c)) why = N48_RP_REC_MODE;
    /* T450 hole 1 (MIB-A1-PROVENANCE.md Q3): a copy that actually WROTE BYTES invalidates whatever an old entry vouched
     * for at that VA / VRAM, even when THIS copy cannot itself be recorded (failed verification, wrong owner, bad mode,
     * missing re-tile, ...). This drop must happen BEFORE the refusal return below, not only on the success path, or a
     * later copy that overwrote a resource but failed one of the checks above leaves the OLD entry still vouching for
     * bytes that are no longer there. `c->ctx`/`c->bytes` may be invalid here (that is one of the very failures being
     * superseded), so sameVa is gated on c->ctx being nonzero and overVram on c->bytes being nonzero; every table entry
     * always has a nonzero ctx (NO_CTX never lets one in), so an invalid c->ctx==0 simply never matches sameVa. */
    if (c && c->copied == 1u && t) {
        for (uint32_t k = 0; k < t->n && k < N48_RP_MAX; ) {
            const n48_rp_ent *e = &t->e[k];
            const int sameVa = c->ctx != 0ull && e->ctx == c->ctx && e->va == c->va;
            const int overVram = c->bytes != 0ull && e->vram < c->vram + c->bytes && c->vram < e->vram + e->bytes;
            if (sameVa || overVram) n48_rp_drop_at(t, k); else k++;
        }
    }
    if (why) { if (t) t->refused[why]++; return why; }
    const uint32_t cap = on == 1u ? N48_RP_CAP_ON : N48_RP_CAP_OFF;   /* build 0.0.552 (switch 109) */
    if (on == 1u && t->n >= cap) {
        const uint32_t k = n48_rp_evict_pick(t, curEpoch);
        if (k < t->n) { n48_rp_drop_at(t, k); t->evicted++; }
    }
    if (t->n >= cap || t->n >= N48_RP_MAX) {   /* not recorded = not proven */
        if (on == 2u && n48_rp_on_would_fit(t, curEpoch)) t->fullOnWould++;   /* SHADOW: counted, nothing else */
        t->refused[N48_RP_REC_FULL]++;
        return N48_RP_REC_FULL;
    }
    n48_rp_ent *e = &t->e[t->n++];
    e->ctx = c->ctx; e->va = c->va; e->vram = c->vram; e->bytes = c->bytes; e->mode = c->mode;
    e->arm = c->arm; e->epoch = c->epoch; e->elemBytes = c->elemBytes;
    e->lin = c->lin ? 1u : 0u; e->w = c->w; e->h = c->h;   /* build 0.0.486 */
    e->known = c->lin >= N48_RP_LIN_KNOWN0 ? c->lin - N48_RP_LIN_KNOWN0 + 1u : 0u;   /* build 0.0.493 */
    t->recorded++;
    return N48_RP_REC_OK;
}
/* 0.0.551's record, unchanged in behaviour: capacity N48_RP_CAP_OFF, no eviction. */
static inline uint32_t n48_rp_record(n48_rp *t, const n48_rp_copy *c)
{
    return n48_rp_record_x(t, c, 0u, 0u);
}

/* The asking frame's own VM: 1 and *vram = the VRAM offset of the 4 KiB page holding `va`, or 0 (unmapped, host page, outside VRAM). */
typedef int (*n48_rp_walk)(const void *vm, uint64_t va, uint64_t *vram);

/* build 0.0.451 item 2 (S4): `askElemBytes` is the T#'s OWN declared element size at the ask (from its gfx10
 * format, xlat12_desc.h's xlat12_format_elem_bytes, carried through ex->desc_tiled_ok - see AppleHardwareHook.cpp's
 * gfxsrc_desc_tiled_ok). A mismatch against the entry's OWN recorded elemBytes refuses (elemMismatch, counted
 * separately from `stale`): the pitch-only shape check can admit two DIFFERENT real bpps at the SAME (mode, VA) key
 * shape (a 128 bpp 4KB_D_X resource with w <= 16 also satisfies the 64 bpp kind's pitch; a 16 bpp resource with
 * w <= 32 also satisfies the 32 bpp kind's) - this is the SECOND, INDEPENDENT check that catches it AT THE ASK,
 * not merely at record time (where the collision is invisible - both hypotheses can genuinely pass the shape
 * check on their OWN terms). */
static inline int n48_rp_ok(n48_rp *t, uint64_t ctx, uint64_t va, uint32_t mode, uint32_t askElemBytes, uint32_t arm,
                            uint32_t epoch, n48_rp_walk walk, const void *vm)
{
    t->asked++;
    if (!ctx) return 0;
    for (uint32_t k = 0; k < t->n && k < N48_RP_MAX; k++) {
        const n48_rp_ent *e = &t->e[k];
        if (e->ctx != ctx || e->va != va || e->mode != mode) continue;
        if (e->lin) { t->linNoT++; return 0; }   /* build 0.0.486: a backing-sourced entry answers only n48_rp_ok_t */
        if (e->elemBytes != askElemBytes) { t->elemMismatch++; return 0; }
        if (e->arm != arm || e->epoch != epoch) { t->stale++; return 0; }
        if (!walk || !vm) { t->walkRefused++; return 0; }
        for (uint64_t p = 0; p < e->bytes; p += 4096ull) {
            uint64_t v = ~0ull;
            if (walk(vm, va + p, &v) != 1 || v != e->vram + p) { t->walkRefused++; return 0; }
        }
        t->proven++;
        return 1;
    }
    return 0;
}

/* T450 hole 4 (MIB-A1-PROVENANCE.md Q3): THE DECIDE -> COMMIT CARRY, a PURE function behind a selector no caller reads yet
 * (nothing in this build wires it - that is the kext-wiring step named in T450's brief, "do not wire it"). An entry
 * recorded while the arm level was DECIDE may answer at COMMIT, but ONLY if the epoch is unchanged since it was recorded
 * (gXdDpEpoch as the kext will pass it) and nothing has superseded it - an ordinary table lookup already enforces "not
 * superseded" (a dropped entry is simply not found by the loop below, same as n48_rp_ok). `decideArm`/`commitArm` are
 * passed in, not read from gfx_src_decide.h, so this header stays free of that include exactly like the rest of the
 * file (N48_SD_ARM_DECIDE == 1, N48_SD_ARM_COMMIT == 2 today; gfx_neuter.h and ws_valid.h mirror the same constants
 * locally with the same comment). `carry` 0 makes armOk reduce to EXACTLY `e->arm == arm` - byte-identical to n48_rp_ok
 * for every argument, which is the default (selector off) behaviour T450 requires. n48_rp_ok itself is UNCHANGED: this
 * is a sibling, not a replacement, so AppleHardwareHook.cpp's existing call keeps working unmodified. */
static inline int n48_rp_ok_carry(n48_rp *t, uint64_t ctx, uint64_t va, uint32_t mode, uint32_t askElemBytes,
                                  uint32_t arm, uint32_t epoch, n48_rp_walk walk, const void *vm, uint32_t carry,
                                  uint32_t decideArm, uint32_t commitArm)
{
    t->asked++;
    if (!ctx) return 0;
    for (uint32_t k = 0; k < t->n && k < N48_RP_MAX; k++) {
        const n48_rp_ent *e = &t->e[k];
        if (e->ctx != ctx || e->va != va || e->mode != mode) continue;
        if (e->lin) { t->linNoT++; return 0; }   /* build 0.0.486: a backing-sourced entry answers only n48_rp_ok_t */
        if (e->elemBytes != askElemBytes) { t->elemMismatch++; return 0; }   // item 2 (S4): same check as n48_rp_ok
        const int armOk = (e->arm == arm) || (carry != 0u && arm == commitArm && e->arm == decideArm);
        if (!armOk || e->epoch != epoch) { t->stale++; return 0; }
        if (!walk || !vm) { t->walkRefused++; return 0; }
        for (uint64_t p = 0; p < e->bytes; p += 4096ull) {
            uint64_t v = ~0ull;
            if (walk(vm, va + p, &v) != 1 || v != e->vram + p) { t->walkRefused++; return 0; }
        }
        t->proven++;
        return 1;
    }
    return 0;
}

/* unmapVA of context `ctx` (0: every context), EVERY entry of it, whatever address range was unmapped. Kept as a
 * sibling (T450's own convention: n48_rp_ok/n48_rp_ok_carry are the same shape) for a caller with no range to give
 * n48_rp_unmap_rng below - AppleHardwareHook.cpp's own unmapVA hook always has one (gfxsrc_desc_unmap's own va/size
 * parameters), so it uses that instead; this function is otherwise unused in the kext today. */
static inline void n48_rp_unmap(n48_rp *t, uint64_t ctx)
{
    t->unmaps++;
    for (uint32_t k = 0; k < t->n && k < N48_RP_MAX; ) if (!ctx || t->e[k].ctx == ctx) n48_rp_drop_at(t, k); else k++;
}
/* build 0.0.451 item 9 (reviewer-confirmed via decide43: AL's two correctly re-tiled and RECORDED entries,
 * #60/#61, were dropped moments later by unmaps of the COPIES' OWN SOURCE BUFFERS - a different VA range entirely
 * - because the context-wide n48_rp_unmap above drops EVERY entry of the context on ANY unmap, address ignored).
 * RANGE-SCOPED: drops an entry of `ctx` only when [e->va, e->va + e->bytes) overlaps the unmapped [va, va + size).
 * `size` 0 means "the whole context" (scope unknown - matches n48_dl_unmap_rng's OWN convention, gfx_desc_port.h),
 * dropping every entry of `ctx` exactly as n48_rp_unmap does.
 * SAFE (per the brief's own requirement): confirmed by n48_rp_ok/n48_rp_ok_carry's OWN walk, lines 490-493/524-527
 * above (`for (uint64_t p = 0; p < e->bytes; p += 4096ull) { ... if (walk(vm, va + p, &v) != 1 || v != e->vram + p)
 * { t->walkRefused++; return 0; } }`) - an entry THIS unmap does not overlap and therefore keeps is still re-proven
 * against the ASKING FRAME'S OWN CURRENT PAGE TABLE at every single ask, never merely trusted because it survived
 * an unmap. A later, unrelated unmap that this range check correctly ignores can never make a kept entry answer
 * yes for pages it no longer maps: the walk would simply fail then, refusing the ask exactly as it always has for
 * a genuinely stale entry. Range-scoping only stops dropping entries this SPECIFIC unmap provably never touched;
 * it does not relax what has to be true for an ask to succeed. */
static inline void n48_rp_unmap_rng(n48_rp *t, uint64_t ctx, uint64_t va, uint64_t size)
{
    t->unmaps++;
    for (uint32_t k = 0; k < t->n && k < N48_RP_MAX; ) {
        const n48_rp_ent *e = &t->e[k];
        const int inCtx = !ctx || e->ctx == ctx;
        const int overlaps = !size || (e->va < va + size && va < e->va + e->bytes);
        if (inCtx && overlaps) n48_rp_drop_at(t, k); else k++;
    }
}
/* WindowServer's binding is now `bound` (0: none): every entry of any other context goes. */
static inline void n48_rp_rebind(n48_rp *t, uint64_t bound)
{
    t->sweeps++;
    for (uint32_t k = 0; k < t->n && k < N48_RP_MAX; ) {
        if (!bound || t->e[k].ctx != bound) { n48_rp_drop_at(t, k); t->sweepDropped++; } else k++;
    }
}

/* build 0.0.523 (RING-NEUTER-FORGIVE.md item 12(d)) — THE MID-ARM LINE, every 512th judged frame, at most 16 per arm,
 * printed under gXdLock after the pending queue's drain. Bounded under 511 bytes by tests/gfx_cgredo_test.cpp. args: frame,
 * table n (u32), asked, proven, stale, walkRefused, recorded, refused[UNVERIFIED], sweeps, sweepDropped, then the pending
 * queue's queued, drained, raced, dropped (u64). */
#define N48_RP_MID_FMT \
    "resprov-mid: f%llu table %u asked %llu proven %llu stale %llu walk-refused %llu recorded %llu unverified %llu sweeps " \
    "%llu dropped %llu pend q %llu dr %llu raced %llu full %llu"

/* ---- 4. the pending queue -------------------------------------------------------------------------------------------------
 * hp6: 434 of 1267 copies lost the ledger's try-lock and were never recorded. A verified copy whose lock is busy is now PUSHED here
 * (lock free, any number of producers) and DRAINED under the ledger's lock by its next holder (the next copy, the decide path
 * before it can ask, an unmap or a WindowServer drop before it clears). Only a full queue drops a record, and that is counted.
 * The copy captures, at copy time, the ledger's arm level, epoch and its place in the CLEAR LOG below.
 *: that place used to be a single table-wide GENERATION counter, and hp7 measured what that costs - `raced 391` of 424
 * queued copies, because `unmaps 1061, rebinds 829` over the boot move a table-wide counter constantly and a whole-frame
 * gXdLock hold means a queued copy almost never wins. The check is now SCOPED: a queued copy is refused only if a clear event
 * since it was copied could have covered ITS context and ITS VA range. Order: drained in push (ticket) order. */
#define N48_RP_PEND 16u
enum { N48_RP_PS_FREE = 0, N48_RP_PS_FILL = 1, N48_RP_PS_READY = 2 };
typedef struct {
    uint32_t state, pad0;        /* N48_RP_PS_* */
    uint64_t clr;                /*: clear events published when this copy was taken (n48_rp_clog.next) */
    uint64_t ticket, copyNo;
    n48_rp_copy c;               /* arm and epoch captured at the copy; ownerWs/ctx filled at the drain from who below */
    int32_t pid;                 /* the copying process */
    uint32_t bound, wsSeq;       /* WindowServer's binding at the copy (ws_ident.h), taken under its own lock */
    int32_t wsPid;
    uint32_t pad;
} n48_rp_pend;
typedef struct {
    n48_rp_pend s[N48_RP_PEND];
    uint64_t nextTicket, queued, drained, raced, dropped;
} n48_rp_pq;

/* Producer: 1 = queued, 0 = the queue is full (the record is DROPPED and counted). */
static inline int n48_rp_pq_push(n48_rp_pq *q, const n48_rp_pend *it)
{
    for (uint32_t i = 0; i < N48_RP_PEND; i++) {
        uint32_t exp = N48_RP_PS_FREE;
        if (!__atomic_compare_exchange_n(&q->s[i].state, &exp, N48_RP_PS_FILL, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) continue;
        n48_rp_pend *d = &q->s[i];
        d->clr = it->clr; d->copyNo = it->copyNo; d->c = it->c; d->pid = it->pid; d->bound = it->bound; d->wsSeq = it->wsSeq;
        d->wsPid = it->wsPid; d->pad = 0u; d->pad0 = 0u;
        d->ticket = __atomic_fetch_add(&q->nextTicket, 1ull, __ATOMIC_RELAXED);
        __atomic_store_n(&d->state, N48_RP_PS_READY, __ATOMIC_RELEASE);
        __atomic_fetch_add(&q->queued, 1ull, __ATOMIC_RELAXED);
        return 1;
    }
    __atomic_fetch_add(&q->dropped, 1ull, __ATOMIC_RELAXED);
    return 0;
}
/* Consumer (ONE at a time: the caller holds the ledger's lock): move every READY item out, oldest ticket first, into out[max].
 * Returns the count. A slot still being filled is left for the next drain. */
static inline uint32_t n48_rp_pq_take(n48_rp_pq *q, n48_rp_pend *out, uint32_t max)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < N48_RP_PEND && n < max; i++) {
        if (__atomic_load_n(&q->s[i].state, __ATOMIC_ACQUIRE) != N48_RP_PS_READY) continue;
        out[n++] = q->s[i];
        __atomic_store_n(&q->s[i].state, N48_RP_PS_FREE, __ATOMIC_RELEASE);
    }
    for (uint32_t i = 1; i < n; i++)                          /* insertion sort by ticket (n <= 16) */
        for (uint32_t j = i; j > 0 && out[j - 1].ticket > out[j].ticket; j--) { n48_rp_pend t = out[j]; out[j] = out[j - 1]; out[j - 1] = t; }
    q->drained += n;
    return n;
}
/* ---- 4b. the clear log, per resource / VA -------------------------------------------------------------------------------
 * Every clear the ledger takes is PUBLISHED here with its scope, and a queued copy asks only about the events that landed after it.
 *   kind 1 = AMDHWVMContext::unmapVA of context `ctxSeq` over [va, va + size) - the hook's own two arguments (hook_unmapVA(self,
 *           va, size)), which the table-wide counter threw away. size 0 = the whole context (scope unknown -> covers everything
 *           of that context).
 *   kind 2 = WindowServer's binding was dropped / rebound, or any whole-table clear: covers EVERY context and VA.
 * Fail-closed on any doubt: the log wrapped past the copy's place, a slot was being rewritten while read, or either side's
 * context / range is unknown (0) -> the copy is refused, exactly as a table-wide counter would have.
 * The publisher runs with or without the ledger's lock (an unmapVA that finds gXdLock busy still publishes), so each slot is a
 * seqlock: `stamp` is stored 0, the fields are written, then `stamp` is stored to the event's number + 1. A reader that does not
 * see the same stamp before and after the fields treats the event as unknown, which refuses. */
#define N48_RP_CLR 64u
enum { N48_RP_CLR_UNMAP = 1u, N48_RP_CLR_WS = 2u };
enum { N48_RP_RACE_NONE = 0u, N48_RP_RACE_WRAP = 1u, N48_RP_RACE_TORN = 2u, N48_RP_RACE_WS = 3u, N48_RP_RACE_VA = 4u };
typedef struct {
    uint64_t stamp;              /* the event's number + 1, published LAST. 0 = being written / never written */
    uint64_t va, size;
    uint32_t kind, ctxSeq;
} n48_rp_clr;
typedef struct {
    n48_rp_clr s[N48_RP_CLR];
    uint64_t next;               /* clear events published so far; event n lives in slot n % N48_RP_CLR */
} n48_rp_clog;

/* Publish one clear. Any thread, with or without the ledger's lock. Returns the log's new size. */
static inline uint64_t n48_rp_clr_push(n48_rp_clog *g, uint32_t kind, uint32_t ctxSeq, uint64_t va, uint64_t size)
{
    const uint64_t n = __atomic_fetch_add(&g->next, 1ull, __ATOMIC_ACQ_REL);
    n48_rp_clr *d = &g->s[n % N48_RP_CLR];
    __atomic_store_n(&d->stamp, 0ull, __ATOMIC_SEQ_CST);      /* readers must not trust the fields while we write them */
    d->kind = kind; d->ctxSeq = ctxSeq; d->va = va; d->size = size;
    __atomic_store_n(&d->stamp, n + 1ull, __ATOMIC_SEQ_CST);
    return n + 1ull;
}
/* Where the log stands now - what a copy records so a later drain knows which events it has not seen. */
static inline uint64_t n48_rp_clr_mark(const n48_rp_clog *g)
{
    return __atomic_load_n(&g->next, __ATOMIC_ACQUIRE);
}
/* 1 = no clear published since `since` could have covered (ctxSeq, [va, va + bytes)). 0 = refuse, and *why is N48_RP_RACE_*. */
static inline int n48_rp_clr_clean(const n48_rp_clog *g, uint64_t since, uint32_t ctxSeq, uint64_t va, uint64_t bytes,
                                   uint32_t *why)
{
    uint32_t w = N48_RP_RACE_NONE;
    const uint64_t now = __atomic_load_n(&g->next, __ATOMIC_ACQUIRE);
    if (now < since || now - since > N48_RP_CLR) { if (why) *why = N48_RP_RACE_WRAP; return 0; }
    for (uint64_t n = since; n < now; n++) {
        const n48_rp_clr *e = &g->s[n % N48_RP_CLR];
        const uint64_t s1 = __atomic_load_n(&e->stamp, __ATOMIC_SEQ_CST);
        const uint32_t kind = __atomic_load_n(&e->kind, __ATOMIC_SEQ_CST), cs = __atomic_load_n(&e->ctxSeq, __ATOMIC_SEQ_CST);
        const uint64_t eva = __atomic_load_n(&e->va, __ATOMIC_SEQ_CST), esz = __atomic_load_n(&e->size, __ATOMIC_SEQ_CST);
        const uint64_t s2 = __atomic_load_n(&e->stamp, __ATOMIC_SEQ_CST);
        if (s1 != n + 1ull || s2 != s1) { w = N48_RP_RACE_TORN; break; }           /* overwritten or half written: unknown */
        if (kind != N48_RP_CLR_UNMAP) { w = N48_RP_RACE_WS; break; }               /* a drop / whole clear covers everything */
        if (cs && ctxSeq && cs != ctxSeq) continue;                                /* another context: cannot touch this VA */
        if (esz && bytes && (eva + esz <= va || va + bytes <= eva)) continue;      /* disjoint ranges */
        w = N48_RP_RACE_VA; break;                                                 /* this VA (or an unknown scope): refuse */
    }
    if (why) *why = w;
    return w == N48_RP_RACE_NONE ? 1 : 0;
}
/* 1 = this queued copy may still be recorded: no clear scoped to ITS context and VA range landed since it was copied. */
static inline int n48_rp_pend_fresh(const n48_rp_pend *it, const n48_rp_clog *g, uint32_t *why)
{
    if (!it || !g) { if (why) *why = N48_RP_RACE_WRAP; return 0; }
    return n48_rp_clr_clean(g, it->clr, it->wsSeq, it->c.va, it->c.bytes, why);
}
/* WHO for a queued copy, from the binding it captured: fills ownerWs and ctx exactly as the locked path does. `wsKey` is the bound
 * context's key (live + unmap-hooked, else 0), computed by the caller under the ledger's lock from it->wsSeq. */
static inline void n48_rp_pend_who(n48_rp_pend *it, uint64_t wsKey)
{
    it->c.ownerWs = (it->bound && it->pid > 0 && it->wsPid == it->pid) ? 1u : 0u;
    it->c.ctx = it->c.ownerWs ? wsKey : 0u;
}

/* ---- 5. the PROVENANCE refusal line ---------------------------------------------------------------------------------
 * hp7 logged `segments refused IB_ERR_DESC 69 (PROVENANCE 69)` and not one VA, so "one of the 69 is F3's own texture at
 * 0x400006000" stayed SUSPECTED. The format lives here, with its argument list, so tests/ws_resprov_test.cpp can prove that it
 * names the surface VA and that the worst case fits the kext's 512-byte line cap. `path` 2 = a class-19 table record, 1 = an
 * inline record. `key` is the fragment program's shader-cache key from the identity memo (0 = the memo does not hold it). */
#define N48_RP_PROV_LINE_CAP 512u
#define N48_RP_PROV_FMT "gfx-xlat:   descriptor PROVENANCE REFUSED: surface VA %#llx gfx12 SW_MODE %u (%s record), " \
                        "fragment program VA %#llx key %#018llx identity %d, context %llu, frame %llu seg %u"
#define N48_RP_PROV_ARGS(va, mode, path, psva, key, id, ctx, frame, seg) \
    (unsigned long long)(va), (unsigned)(mode), (path) == 2u ? "table" : "inline", (unsigned long long)(psva), \
    (unsigned long long)(key), (int)(id), (unsigned long long)(ctx), (unsigned long long)(frame), (unsigned)(seg)

/* Page-out symmetry: 1 = refuse this page-out (keep the skip). Each flag is sticky for the boot, NOT the switch, so
 * turning a switch off after a re-tile cannot let gfx12 bytes reach system memory.
 * build 0.0.450 item 2 (T450 open doubt 2, CONDUCTOR FIX after the first pass): EXTENDED TO EVERY GOLDEN-TESTED
 * KIND, but EXACTLY - PER GFX10 MODE, not one flag for the whole boot. The first pass shared ONE sticky flag across
 * both modes, which meant switch 46 OFF (256B_D never re-tiled at all: nothing ever calls n48_rp_retile_kind with
 * N48_RP_KIND_256B_D_8, since that requires the switch) could still see `retiled256bd` come up true from the
 * PRE-EXISTING 32bpp 4KB_D_X path alone (switch 11 only) - refusing 256B_D page-outs that were never re-tiled, a
 * behaviour change with nothing to protect. Two independent flags fix this: `retiled4kbdx` covers mode 22 (set by
 * the always-on 32bpp path AND, under switch 46, the 8/64bpp kinds - all three write bytes in 4KB_D_X's gfx10 order
 * page-in, so all three need the SAME symmetry); `retiled256bd` covers mode 2 (set ONLY by the 256B_D kind, which
 * itself only ever runs under switch 46). With switch 46 OFF, `retiled256bd` can never become 1 (nothing sets it),
 * so this reduces to EXACTLY 0.0.449's rule: mode 22 refused after any 4KB_D_X re-tile, mode 2 never refused. */
static inline int n48_rp_pageout_refuse(uint32_t retiled4kbdx, uint32_t retiled256bd, uint32_t surf)
{
    const uint32_t m = surf & 0x1fu;
    return (retiled4kbdx && m == N48_RP_G10_4KB_D_X) || (retiled256bd && m == N48_RP_G10_256B_D);
}

/* ---- 6. build 0.0.486 - THE BACKING-SOURCED COPY (switch 59, DEFAULT OFF; an internal design note Q2-Q6) -----------
 * GENERALISED BY build 0.0.492: every VRAM-side mode the 41 run10f step-0 records show, the correct
 * format pairing, a STREAMED copy past N48_RP_MAX_BYTES, and priority over the gfx10 re-tiles. See "0.0.492" below.
 * WHY. The login screen's static textures (the user picture, 144x144 8_8_8_8; the name/clock mask, 209x41 8_UNORM; the
 * wallpaper 1920x1080 at 0x405800000; the fp16 LUTs) are refused provenance because the residency copy treated their
 * system-memory bytes as a gfx10-tiled image. found that premise wrong: Apple copies them (pageTextureWithSurfaceCopy /
 * pageTextureWithStretch) between TWO surface descriptions, and the system-memory side has its OWN layout, read by
 * AMDAccelResource::fillUBMSurfaceInfoBacking from the level-0 sub-record
 * r14 = *(res+0x180) + 8 + level * 0x40 (CONFIRMED 0xbdd3b76 `movq 0x180(%rdi),%rax` .. 0xbdd3b8c `addq $0x8,%r14`):
 *   base  = map VA + *(u64 *)(rec+0x08)            0xbdd3b98 `addq (%r14),%r15` .. 0xbdd3cf3 `movq %r15,0x18(%rbx)`
 *   w, h  = rec+0x00, rec+0x02 (u16)               0xbdd3c2f `movzwl (%rdi),%r12d`, 0xbdd3c43 `movzwl 0x2(%rdi),%r14d`
 *                                                  (the branch taken when res+0xe0 is 0 and the format is not block-compressed)
 *   pitch = rec+0x3a (u16) IN ELEMENTS             0xbdd3d2c `movzwl 0x32(%rdx),%eax` -> surfinfo+0x64; the Addr2 override
 *                                                  multiplies it by the element size: 0xbddcb65 `movl 0x64(%rbx),%eax` ..
 *                                                  0xbddcb9a `shll %cl,%eax` (cl = format-table +8 [11:9]) -> surfinfo+0x78
 *   swizzle = rec+0x44 [9:4], type = rec+0x44 [11:10]   0xbddcb14 `movl 0x44(%r15,%r12),%esi; shrl $4; andl $0x3f` and
 *                                                  0xbddcb35 `shrl $0xa; andl $3` through the hardware's vtable 0x518 /
 *                                                  0x520; AMDHardware::getUbmSwizzleMode / getUbmResourceType are identity
 *                                                  (0xbe2f5da / 0xbe2f5e2 `movl %esi,%eax`), so the values are addrlib's own
 *                                                  enums: swizzle 0 = ADDR_SW_LINEAR, type 1 = ADDR_RSRC_TEX_2D (that slots
 *                                                  0x518/0x520 ARE those two functions is SUSPECTED from the slot order).
 * The VRAM side keeps the record's +0x40 ([4:0] swizzle, [6:5] type;).
 *
 * 0.0.492 - WHAT CHANGED (every item a field read or a golden-tested equation; nothing keys on a VA or a constant of one texture):
 *  (1) THE BACKING. (CONFIRMED on all 41 run10f records): Apple writes +0x44 = 0x601 (swizzle 32 = ADDR_SW_LINEAR_GENERAL,
 *      a linear layout whose pitch is a whole number of elements) whenever the row bytes are not a multiple of 256, exceed 0x2000
 *      or rec+0x08's low byte is non-zero, else 0x400 (swizzle 0 = ADDR_SW_LINEAR). Both are row-major with the pitch at rec+0x3a
 *      in elements: both are accepted; nothing else is.
 *  (2) THE VRAM SIDE. Any gfx10 mode in kN48RpLinModes: 22 (ADDR_SW_4KB_D_X), 2 (ADDR_SW_256B_D), 27 (ADDR_SW_64KB_R_X) - the
 *      three the 41 records show - written as the gfx12 mode xlat12_desc.h's kXlat12SwModeG10ToG12 maps each to (2 ADDR3_4KB_2D,
 *      1 ADDR3_256B_2D, 3 ADDR3_64KB_2D: the table test pins the equality), so the T# the translator places describes exactly
 *      the layout written. The map is one-to-one on this set, so an entry's gfx12 mode names its gfx10 mode for the ask.
 *  (3) THE FORMAT. 0.0.486 demanded rec+0x07 == res+0x1b4, which compares two DIFFERENT _ati_format_info_table index kinds and
 *      can never pass. Now: both indices must be in kN48RpFmt (the table's own bytes, cited per row), the resource's a
 *      texture format and the backing's a RAW element, with the SAME bytes per element (dword +8 [11:9], the field Apple's own
 *      backing fill reads at 0xbddcb7a / 0xbddcb94). Anything else refuses.
 *  (4) THE LENGTH. The gfx12 image is written one CHUNK of block rows at a time (n48_rp_lin_chunk / n48_rp_lin_band): each
 *      chunk's source span and gfx12 bytes are at most N48_RP_MAX_BYTES; the total is bounded by the destination's own length and
 *      N48_RP_LIN_MAX_TOTAL. A texture whose single block row exceeds the chunk bound refuses (N48_RP_LIN_BAND).
 *  (5) PRIORITY. N48_RP_LIN_TODAY (0.0.486's disjointness from the gfx10 re-tiles) is gone: with switch 59 ON a record whose
 *      backing is linear takes THIS path, and the gfx10 re-tiles refuse it (n48_rp_lin_blocks_old), because they were built on
 *      the premise that the system-memory bytes are gfx10-tiled (: they never were). Switch 59 OFF: 0.0.491 exactly.
 *
 * THE REFUSALS, in check order (each keeps today's copy, byte for byte): n48_rp_lin_check below. */
enum {
    N48_RP_LIN_OK = 0, N48_RP_LIN_NIBBLE = 1, N48_RP_LIN_REC = 2, N48_RP_LIN_VRAM_SWZ = 3, N48_RP_LIN_NOT_LINEAR = 4,
    N48_RP_LIN_BK_TYPE = 5, N48_RP_LIN_FMT = 6, N48_RP_LIN_DIMS = 7, N48_RP_LIN_SLICES = 8, N48_RP_LIN_MIP = 9,
    N48_RP_LIN_E0 = 10, N48_RP_LIN_BPE = 11, N48_RP_LIN_BKOFF = 12, N48_RP_LIN_PITCH = 13, N48_RP_LIN_SRC = 14,
    N48_RP_LIN_BAND = 15, N48_RP_LIN_DST = 16, N48_RP_LIN_REASONS = 17
};
static inline const char *n48_rp_lin_name(uint32_t r)
{
    static const char *const k[N48_RP_LIN_REASONS] = {
        "ok", "nibble not 1/8", "record unreadable", "VRAM side not 2D SW 2/22/27", "backing not LINEAR (SW 0/32)",
        "backing not 2D", "format pair not raw/same bpe", "dims differ", "not one slice", "first level not 0", "res+0xe0 set",
        "rowBytes not the VRAM pitch", "backing offset", "pitch", "extent past source", "block row past the chunk bound",
        "gfx12 length past dstLen"
    };
    return r < N48_RP_LIN_REASONS ? k[r] : "?";
}
#define N48_RP_LIN_MAX_TOTAL (64ull << 20)   /* == Navi48AccelPeer.cpp kCopyMaxBytes (static_assert there) */
/* build 0.0.544 item 4a: ONE block row may exceed the 1 MiB chunk target. At 2560 wide one 64KB_2D row at 4 B per
 * element is 20 x 64 KiB = 1,310,720 B, so the 1440p wallpaper refused N48_RP_LIN_BAND (`block row past the chunk bound`) and was
 * neither re-tiled nor recorded. A chunk is still as many whole block rows as fit in N48_RP_MAX_BYTES; when not even one fits, the
 * chunk is exactly ONE block row, and that row (its gfx12 bytes and its source span) may be up to N48_RP_LIN_BAND_MAX. The only
 * buffers sized by a chunk are the stream's two heap chunk buffers (Navi48AccelPeer.cpp rp_lin_stream_make, IOMalloc, freed on every
 * exit); the write loop and the SDMA staging pull 256-byte batches through rp_lin_fetch (fastcopy.h N48_FC_BATCH_BYTES), so their
 * 1 MiB buffers (kCopyChunkBytes, N48_FC_STAGING_BYTES, switch 89's read-back) never hold a chunk. 2 MiB covers a 4096-wide 32bpp
 * row (3840 wide = 1.875 MiB); anything wider still refuses BAND. Every chunk whose band fits 1 MiB is planned exactly as before. */
#define N48_RP_LIN_BAND_MAX  (2u << 20)

/* 6a. The gfx12 2D single-sample equations, generic over the block size (256 B, 4 KiB, 64 KiB) and the element size (1-16 B).
 * Transcribed from the vendored addrlib's gfx12SwizzlePattern.h (re/graphics/src/mesa/src/amd/addrlib/src/gfx12):
 * GFX12_SW_256B_2D_1xAA_PATINFO {b,0,0,0}, GFX12_SW_4KB_2D_1xAA_PATINFO {b,b+1,0,0}, GFX12_SW_64KB_2D_1xAA_PATINFO {b,b+1,b+1,0}
 * (b = log2 bytes per element) index GFX12_SW_PATTERN_NIBBLE1 (address bits 0-7), NIBBLE2 (8-11), NIBBLE3 (12-15). No XOR term
 * at all: gfx12 has no pipe/bank hash in the equation. The block index above the block is row-major with the pitch in blocks,
 * exactly as n48_rp_g12_off. Golden-tested (ws_resprov_test.cpp L1): every row of kG12, kG12_4kbdx8, kG12_4kbdx64, kG12_256d8,
 * kG12_256d64, kG12_64k32 and kG12Real (the records' own dims), and equal to the four hand-written gfx12 functions above. */
#define N48_G12X(i) ((uint8_t)(0x10u | (i)))
#define N48_G12Y(i) ((uint8_t)(0x20u | (i)))
static const uint8_t kN48G12Nib1[5][8] = {
    { N48_G12X(0), N48_G12X(1), N48_G12Y(0), N48_G12X(2), N48_G12Y(1), N48_G12Y(2), N48_G12X(3), N48_G12Y(3) },   /* row 0 */
    { 0,           N48_G12X(0), N48_G12Y(0), N48_G12X(1), N48_G12Y(1), N48_G12X(2), N48_G12Y(2), N48_G12X(3) },   /* row 1 */
    { 0,           0,           N48_G12X(0), N48_G12Y(0), N48_G12X(1), N48_G12Y(1), N48_G12X(2), N48_G12Y(2) },   /* row 2 */
    { 0,           0,           0,           N48_G12X(0), N48_G12Y(0), N48_G12X(1), N48_G12X(2), N48_G12Y(1) },   /* row 3 */
    { 0,           0,           0,           0,           N48_G12X(0), N48_G12Y(0), N48_G12X(1), N48_G12Y(1) },   /* row 4 */
};
static const uint8_t kN48G12Nib2[5][4] = {   /* NIBBLE2 rows 1-5 (row b+1) */
    { N48_G12Y(4), N48_G12X(4), N48_G12Y(5), N48_G12X(5) },   /* row 1 */
    { N48_G12Y(3), N48_G12X(4), N48_G12Y(4), N48_G12X(5) },   /* row 2 */
    { N48_G12Y(3), N48_G12X(3), N48_G12Y(4), N48_G12X(4) },   /* row 3 */
    { N48_G12Y(2), N48_G12X(3), N48_G12Y(3), N48_G12X(4) },   /* row 4 */
    { N48_G12Y(2), N48_G12X(2), N48_G12Y(3), N48_G12X(3) },   /* row 5 */
};
static const uint8_t kN48G12Nib3[5][4] = {   /* NIBBLE3 rows 1-5 (row b+1) */
    { N48_G12Y(6), N48_G12X(6), N48_G12Y(7), N48_G12X(7) },   /* row 1 */
    { N48_G12Y(5), N48_G12X(6), N48_G12Y(6), N48_G12X(7) },   /* row 2 */
    { N48_G12Y(5), N48_G12X(5), N48_G12Y(6), N48_G12X(6) },   /* row 3 */
    { N48_G12Y(4), N48_G12X(5), N48_G12Y(5), N48_G12X(6) },   /* row 4 */
    { N48_G12Y(4), N48_G12X(4), N48_G12Y(5), N48_G12X(5) },   /* row 5 */
};
/* The source of address bit `bit` (0 = none, 0x1i = X bit i, 0x2i = Y bit i) for a 2^blkLog2-byte block at 2^bpeLog2 bytes per
 * element. blkLog2 must be 8, 12 or 16 and bpeLog2 0..4 (else 0). */
static inline uint32_t n48_g12_src(uint32_t blkLog2, uint32_t bpeLog2, uint32_t bit)
{
    if (bpeLog2 > 4u || (blkLog2 != 8u && blkLog2 != 12u && blkLog2 != 16u) || bit >= blkLog2) return 0u;
    if (bit < 8u) return kN48G12Nib1[bpeLog2][bit];
    if (bit < 12u) return kN48G12Nib2[bpeLog2][bit - 8u];
    return kN48G12Nib3[bpeLog2][bit - 12u];
}
/* The block's width and height in elements (log2): the number of X and of Y bits inside the block. 0 = unsupported. */
static inline int n48_g12_blk(uint32_t blkLog2, uint32_t bpeLog2, uint32_t *bwLog2, uint32_t *bhLog2)
{
    uint32_t nx = 0, ny = 0;
    if (bpeLog2 > 4u || (blkLog2 != 8u && blkLog2 != 12u && blkLog2 != 16u)) return 0;
    for (uint32_t b = 0; b < blkLog2; b++) {
        const uint32_t s = n48_g12_src(blkLog2, bpeLog2, b);
        if (s & 0x10u) nx++; else if (s & 0x20u) ny++;
    }
    if (nx + ny + bpeLog2 != blkLog2) return 0;   /* every non-element bit is a coordinate bit, once */
    *bwLog2 = nx; *bhLog2 = ny;
    return 1;
}
/* In-block byte offset of the element (x, y) (only the low bits of x and y inside the block are read). */
static inline uint32_t n48_g12_inblk(uint32_t blkLog2, uint32_t bpeLog2, uint32_t x, uint32_t y)
{
    uint32_t o = 0u;
    for (uint32_t b = 0; b < blkLog2; b++) {
        const uint32_t s = n48_g12_src(blkLog2, bpeLog2, b);
        if (s & 0x10u) o |= ((x >> (s & 0xfu)) & 1u) << b;
        else if (s & 0x20u) o |= ((y >> (s & 0xfu)) & 1u) << b;
    }
    return o;
}
/* Byte offset of the element (x, y) in a single-level gfx12 2D surface, `pb` = pitch in blocks. */
static inline uint64_t n48_g12_off2d(uint32_t blkLog2, uint32_t bpeLog2, uint32_t x, uint32_t y, uint32_t pb)
{
    uint32_t bwl = 0, bhl = 0;
    if (!n48_g12_blk(blkLog2, bpeLog2, &bwl, &bhl)) return 0ull;
    return (((uint64_t)(y >> bhl) * pb + (x >> bwl)) << blkLog2) + n48_g12_inblk(blkLog2, bpeLog2, x, y);
}

/* 6b. The VRAM-side modes this path writes, and the gfx12 mode each becomes (kXlat12SwModeG10ToG12's own mapping: the test pins
 * it). The gfx10 2D block of these modes has the same element dimensions as its gfx12 target's (addrlib: the block's size and the
 * element size alone fix a 2D block's shape), which the VRAM side's rowBytes must show (golden: kG10Pitch). */
static const struct { uint8_t g10, g12, blkLog2; } kN48RpLinModes[] = {
    { 2u,  1u, 8u },    /* ADDR_SW_256B_D   -> ADDR3_256B_2D */
    { 22u, 2u, 12u },   /* ADDR_SW_4KB_D_X  -> ADDR3_4KB_2D  */
    { 27u, 3u, 16u },   /* ADDR_SW_64KB_R_X -> ADDR3_64KB_2D */
};
#define N48_RP_LIN_NMODES 3u
static inline uint32_t n48_rp_lin_g12_mode(uint32_t g10)
{
    for (uint32_t i = 0; i < N48_RP_LIN_NMODES; i++) if (kN48RpLinModes[i].g10 == g10) return kN48RpLinModes[i].g12;
    return 0u;
}
static inline uint32_t n48_rp_lin_g10_for_g12(uint32_t g12)
{
    for (uint32_t i = 0; i < N48_RP_LIN_NMODES; i++) if (kN48RpLinModes[i].g12 == g12) return kN48RpLinModes[i].g10;
    return 0u;
}
static inline uint32_t n48_rp_lin_blk_log2(uint32_t g12)
{
    for (uint32_t i = 0; i < N48_RP_LIN_NMODES; i++) if (kN48RpLinModes[i].g12 == g12) return kN48RpLinModes[i].blkLog2;
    return 0u;
}

/* 6c. _ati_format_info_table (AMDRadeonX6000 __DATA,__const 0xbf47e00, 16-byte entries, dword 0 = index << 8; the Addr2 backing
 * fill indexes it `shll $0x4` and reads dword +8 at 0xbddcb7a, element size log2 = [11:9] at 0xbddcb94/0xbddcb97). The rows the
 * 41 run10f records use, copied from the table's bytes (re/tahoe-26.6.2-x86_64/out/kexts/com.apple.kext.AMDRadeonX6000,
 * file offset 0x18ae00 + 16 x index): d1 = dword +4, d2 = dword +8. `raw` 1 = a backing element format: the class the
 * three backing indices share (d2 [27:20] = 0x09) whose low lane code is the plain single/two-lane code of its size (0x001 as
 * for 0x02, 0x00d as for 0x20/0x22, 0x01d as for 0x23/0x41) - that reading of the class is SUSPECTED; the bytes are CONFIRMED.
 * A resource/backing pair is accepted only when both rows are here, the backing's row is raw and the resource's is not, and
 * [11:9] is equal. */
typedef struct { uint8_t idx, raw; uint32_t d1, d2; } n48_rp_fmt_ent;
static const n48_rp_fmt_ent kN48RpFmt[] = {
    { 0x02u, 0u, 0x00000037u, 0x02100001u },   /* resource: the mask / P's textures, 1 B per element (8_UNORM in their T#s) */
    { 0x0fu, 0u, 0x00000052u, 0x0210041au },   /* resource: 8_8_8_8 (the avatar, F3's window, the wallpaper), 4 B */
    { 0x2bu, 0u, 0x00000007u, 0x0250061fu },   /* resource: the fp16 LUTs (16_16_16_16), 8 B */
    { 0x63u, 1u, 0x00000034u, 0x00900001u },   /* backing: raw 8-bit */
    { 0x65u, 1u, 0x00000024u, 0x0090040du },   /* backing: raw 32-bit */
    { 0x68u, 1u, 0x0000000du, 0x0090061du },   /* backing: raw 64-bit */
};
#define N48_RP_NFMT 6u
static inline const n48_rp_fmt_ent *n48_rp_fmt_row(uint32_t idx)
{
    for (uint32_t i = 0; i < N48_RP_NFMT; i++) if (kN48RpFmt[i].idx == idx) return &kN48RpFmt[i];
    return 0;
}
/* 1 and *bpeLog2 = the shared element size when (fmtRes, fmtBk) is an accepted pair; 0 otherwise. */
static inline int n48_rp_lin_fmt_pair(uint32_t fmtRes, uint32_t fmtBk, uint32_t *bpeLog2)
{
    const n48_rp_fmt_ent *r = n48_rp_fmt_row(fmtRes), *b = n48_rp_fmt_row(fmtBk);
    if (!r || !b || r->raw != 0u || b->raw != 1u) return 0;
    const uint32_t lr = (r->d2 >> 9) & 7u, lb = (b->d2 >> 9) & 7u;
    if (lr != lb || lr > 4u) return 0;
    if (bpeLog2) *bpeLog2 = lr;
    return 1;
}

/* Everything the check reads, as the kext has it BEFORE the copy (Navi48AccelPeer.cpp rp_lin_prepare). */
typedef struct {
    uint32_t nibble;            /* (res+0x1b4 >> 26) & 0xf - the pageTexture flavour */
    uint32_t fmtRes;            /* res+0x1b4 & 0xff - the format index the Addr2 fill looks up (0xbddcb68) */
    uint32_t w, h, depth, rowBytes;   /* res+0xb0 / +0xb2 (u16), +0xb4, +0xb8 - the VRAM side (n48_rp_shape's) */
    uint32_t firstMip;          /* res+0xae (u8): the level shift in the fill (0xbdd3c33 `movzbl 0xae(%r13),%edx`) */
    uint32_t hasE0;             /* res+0xe0 != 0: the fill takes w/h from another path (0xbdd3bc7 `testq %rcx,%rcx`) */
    uint64_t bytes;             /* res+0x230 */
    uint64_t backingOffset;     /* res+0xf8 */
    uint32_t recOk;             /* 1 = res+0x180 is a readable kernel record (the 0x3c8-byte record of) */
    uint32_t bw, bh, rec04;     /* rec+0x00, +0x02 (the backing's w/h), +0x04 (u16, logged only: NOT ESTABLISHED) */
    uint32_t fmtBk;             /* rec+0x07 (u8): the format index the base fill reads (0xbdd3bb5 `movzbl 0x7(%rdi),%edx`) */
    uint64_t off;               /* rec+0x08 (u64): the backing's byte offset */
    uint32_t pitch;             /* rec+0x3a (u16): the backing's pitch in ELEMENTS */
    uint32_t swzVram, swzBk;    /* rec+0x40, rec+0x44 */
    uint64_t srcLen, mdLen, dstLen;   /* source SysMemory +0x40, its backing descriptor's length, destination VidMemory +0x40 */
} n48_rp_lin_in;
/* The conversion, as n48_rp_lin_check proved it. Block rows ("bands") of the gfx12 image are bandBytes long; a CHUNK is chunkRows
 * of them (the last one may be shorter) - the unit the kext reads, converts and writes. */
typedef struct {
    uint32_t g10Mode, g12Mode, blkLog2, bpeLog2, bpe, bwLog2, bhLog2, pbk, rowsBlk, chunkRows;
    uint64_t srcOff, pitchBytes, srcEnd, g12Bytes, bandBytes;
} n48_rp_lin_plan;

/* The VRAM side's gfx10 pitch in elements: `w` aligned to the 2D block width of this block size at this element size (addrlib's
 * pitch alignment for these modes; golden: kG10Pitch at every record's real (mode, bpp, width)). 0 = out of range. */
static inline uint32_t n48_rp_lin_g10_pitch(uint32_t blkLog2, uint32_t bpeLog2, uint32_t w)
{
    if (blkLog2 < bpeLog2 || !w || w > N48_RP_MAX_DIM) return 0u;
    const uint32_t eLog2 = blkLog2 - bpeLog2, bwl = (eLog2 + 1u) / 2u, bw = 1u << bwl;
    return (w + bw - 1u) & ~(bw - 1u);
}

static inline void n48_rp_lin_plan_clear(n48_rp_lin_plan *p)
{
    p->g10Mode = p->g12Mode = p->blkLog2 = p->bpeLog2 = p->bpe = p->bwLog2 = p->bhLog2 = p->pbk = p->rowsBlk = p->chunkRows = 0u;
    p->srcOff = p->pitchBytes = p->srcEnd = p->g12Bytes = p->bandBytes = 0ull;
}

static inline uint32_t n48_rp_lin_check(const n48_rp_lin_in *in, n48_rp_lin_plan *p)
{
    if (p) n48_rp_lin_plan_clear(p);
    if (!in || !p) return N48_RP_LIN_REC;
    if (in->nibble != 1u && in->nibble != 8u) return N48_RP_LIN_NIBBLE;              /* Stretch (1) / SurfaceCopy (8) only */
    if (in->recOk != 1u) return N48_RP_LIN_REC;
    const uint32_t g10 = in->swzVram & 0x1fu, g12 = n48_rp_lin_g12_mode(g10);
    if (!g12 || ((in->swzVram >> 5) & 3u) != 1u) return N48_RP_LIN_VRAM_SWZ;         /* a mode this path writes, 2D */
    const uint32_t bkSw = (in->swzBk >> 4) & 0x3fu;
    if (bkSw != 0u && bkSw != 32u) return N48_RP_LIN_NOT_LINEAR;                    /* ADDR_SW_LINEAR / _LINEAR_GENERAL only */
    if (((in->swzBk >> 10) & 3u) != 1u) return N48_RP_LIN_BK_TYPE;                   /* ADDR_RSRC_TEX_2D */
    uint32_t bpeLog2 = 0u;
    if (!n48_rp_lin_fmt_pair(in->fmtRes, in->fmtBk, &bpeLog2)) return N48_RP_LIN_FMT;
    if (!in->w || !in->h || in->w > N48_RP_MAX_DIM || in->h > N48_RP_MAX_DIM || in->bw != in->w || in->bh != in->h)
        return N48_RP_LIN_DIMS;
    if (in->depth != 1u) return N48_RP_LIN_SLICES;
    if (in->firstMip != 0u) return N48_RP_LIN_MIP;
    if (in->hasE0) return N48_RP_LIN_E0;
    const uint32_t blkLog2 = n48_rp_lin_blk_log2(g12), eb = 1u << bpeLog2;
    uint32_t bwl = 0u, bhl = 0u;
    if (!blkLog2 || !n48_g12_blk(blkLog2, bpeLog2, &bwl, &bhl)) return N48_RP_LIN_VRAM_SWZ;
    /* the VRAM side's own pitch must be this mode's at this element size: the element size is seen twice, independently */
    if ((uint64_t)in->rowBytes != (uint64_t)eb * n48_rp_lin_g10_pitch(blkLog2, bpeLog2, in->w)) return N48_RP_LIN_BPE;
    if (in->backingOffset != 0ull) return N48_RP_LIN_BKOFF;   /* the md offset and the record's offset then coincide */
    if (in->pitch < in->w || in->pitch > N48_RP_MAX_DIM) return N48_RP_LIN_PITCH;
    const uint64_t pb = (uint64_t)in->pitch * eb;
    const uint64_t ext = pb * (uint64_t)(in->h - 1u) + (uint64_t)in->w * eb;         /* <= 16384 x 16 x 16384: no overflow */
    const uint64_t lims[3] = { in->srcLen, in->mdLen, in->bytes };
    for (uint32_t i = 0; i < 3u; i++)
        if (in->off > lims[i] || ext > lims[i] - in->off) return N48_RP_LIN_SRC;
    const uint32_t pbk = (in->w + (1u << bwl) - 1u) >> bwl, rows = (in->h + (1u << bhl) - 1u) >> bhl;
    const uint64_t band = (uint64_t)pbk << blkLog2, bandSrc = pb << bhl;
    if (band > (uint64_t)N48_RP_LIN_BAND_MAX || bandSrc > (uint64_t)N48_RP_LIN_BAND_MAX) return N48_RP_LIN_BAND;   /* 0.0.544 4a */
    const uint64_t g12b = band * rows;
    if (g12b > in->dstLen || g12b > N48_RP_LIN_MAX_TOTAL) return N48_RP_LIN_DST;
    uint64_t cr = (uint64_t)N48_RP_MAX_BYTES / band;
    if ((uint64_t)N48_RP_MAX_BYTES / bandSrc < cr) cr = (uint64_t)N48_RP_MAX_BYTES / bandSrc;
    if (cr == 0u) cr = 1u;   /* build 0.0.544 4a: a band past the 1 MiB target (<= N48_RP_LIN_BAND_MAX) is a chunk of its own */
    if (cr > rows) cr = rows;
    p->g10Mode = g10; p->g12Mode = g12; p->blkLog2 = blkLog2; p->bpeLog2 = bpeLog2; p->bpe = eb; p->bwLog2 = bwl; p->bhLog2 = bhl;
    p->pbk = pbk; p->rowsBlk = rows; p->chunkRows = (uint32_t)cr;
    p->srcOff = in->off; p->pitchBytes = pb; p->srcEnd = in->off + ext; p->g12Bytes = g12b; p->bandBytes = band;
    return N48_RP_LIN_OK;
}

/* 6d. The streamed conversion. The in-block offsets are separable (no XOR term): ox[x mod bw] | oy[y mod bh]. */
typedef struct { uint32_t ox[256], oy[256]; } n48_rp_lin_lut;   /* 256 = the widest/tallest block (64 KiB at 1 B per element) */
static inline int n48_rp_lin_lut_fill(const n48_rp_lin_plan *p, n48_rp_lin_lut *l)
{
    if (!p || !l || p->bwLog2 > 8u || p->bhLog2 > 8u || !p->blkLog2) return 0;
    for (uint32_t i = 0; i < (1u << p->bwLog2); i++) l->ox[i] = n48_g12_inblk(p->blkLog2, p->bpeLog2, i, 0u);
    for (uint32_t i = 0; i < (1u << p->bhLog2); i++) l->oy[i] = n48_g12_inblk(p->blkLog2, p->bpeLog2, 0u, i);
    return 1;
}
/* The chunk holding gfx12 byte `at`: block rows [*j0, *j0 + *nRows), the gfx12 bytes [*dstFrom, *dstFrom + *dstLen) they fill, and
 * the source span [*srcFrom, *srcFrom + *srcLen) (backing offsets, from the image's first byte's offset srcOff) they read. 0 = `at`
 * outside the image or a plan that did not come from n48_rp_lin_check. Every chunk's two lengths are at most N48_RP_MAX_BYTES, or at
 * most N48_RP_LIN_BAND_MAX for a chunk of ONE band (build 0.0.544 4a). */
static inline int n48_rp_lin_chunk(const n48_rp_lin_plan *p, uint32_t w, uint32_t h, uint64_t at, uint32_t *j0, uint32_t *nRows,
                                   uint64_t *srcFrom, uint64_t *srcLen, uint64_t *dstFrom, uint64_t *dstLen)
{
    if (!p || !p->chunkRows || !p->bandBytes || !w || !h || at >= p->g12Bytes) return 0;
    const uint64_t chunkBytes = p->bandBytes * p->chunkRows;
    const uint32_t first = (uint32_t)(at / chunkBytes) * p->chunkRows;
    if (first >= p->rowsBlk) return 0;
    const uint32_t n = (p->rowsBlk - first < p->chunkRows) ? p->rowsBlk - first : p->chunkRows;
    const uint64_t y0 = (uint64_t)first << p->bhLog2;
    uint64_t y1 = (uint64_t)(first + n) << p->bhLog2;
    if (y1 > h) y1 = h;
    if (y0 >= y1) return 0;
    *j0 = first; *nRows = n;
    *dstFrom = (uint64_t)first * p->bandBytes; *dstLen = (uint64_t)n * p->bandBytes;
    *srcFrom = p->srcOff + y0 * p->pitchBytes; *srcLen = (y1 - 1u - y0) * p->pitchBytes + (uint64_t)w * p->bpe;
    /* build 0.0.544 4a: a one-band chunk may reach N48_RP_LIN_BAND_MAX; a chunk of several bands stays within 1 MiB */
    const uint64_t lim = p->chunkRows == 1u ? (uint64_t)N48_RP_LIN_BAND_MAX : (uint64_t)N48_RP_MAX_BYTES;
    return (*dstLen <= lim && *srcLen <= lim) ? 1 : 0;
}
/* Block rows [j0, j0 + nRows) -> `dst` (exactly nRows x bandBytes; padding texels ZERO), from `src` = the backing's bytes starting
 * at source offset srcOff + j0 x bh x pitchBytes (srcBytes of them). Every bound is re-checked: a band that did not come from
 * n48_rp_lin_chunk for this plan writes nothing. 1 or 0. */
static inline int n48_rp_lin_band(const n48_rp_lin_plan *p, const n48_rp_lin_lut *l, uint32_t w, uint32_t h, uint32_t j0,
                                  uint32_t nRows, const uint8_t *src, uint64_t srcBytes, uint8_t *dst, uint64_t dstBytes)
{
    if (!p || !l || !src || !dst || !w || !h || w > N48_RP_MAX_DIM || h > N48_RP_MAX_DIM || !nRows) return 0;
    if (!p->bandBytes || p->bpe != (1u << p->bpeLog2) || p->pitchBytes < (uint64_t)w * p->bpe) return 0;
    if (j0 >= p->rowsBlk || nRows > p->rowsBlk - j0 || dstBytes != (uint64_t)nRows * p->bandBytes) return 0;
    if (((uint64_t)(w - 1u) >> p->bwLog2) >= p->pbk) return 0;
    const uint64_t y0 = (uint64_t)j0 << p->bhLog2;
    uint64_t y1 = (uint64_t)(j0 + nRows) << p->bhLog2;
    if (y1 > h) y1 = h;
    if (y0 >= y1 || (y1 - 1u - y0) * p->pitchBytes + (uint64_t)w * p->bpe > srcBytes) return 0;
    for (uint64_t i = 0; i < dstBytes; i++) dst[i] = 0u;
    const uint32_t bwm = (1u << p->bwLog2) - 1u, bhm = (1u << p->bhLog2) - 1u, eb = p->bpe;
    for (uint64_t y = y0; y < y1; y++) {
        const uint64_t rowBase = ((((y >> p->bhLog2) - j0) * (uint64_t)p->pbk) << p->blkLog2) + l->oy[y & bhm];
        const uint8_t *s = src + (y - y0) * p->pitchBytes;
        for (uint32_t x = 0; x < w; x++) {
            uint8_t *d = dst + rowBase + (((uint64_t)(x >> p->bwLog2)) << p->blkLog2) + l->ox[x & bwm];
            for (uint32_t k = 0; k < eb; k++) d[k] = s[(uint64_t)x * eb + k];
        }
    }
    return 1;
}
/* The whole image, chunk by chunk (host tests; the kext streams the same chunks through n48_rp_lin_band). `src` holds the backing
 * from offset 0 (srcBytes of it), `dst` the whole gfx12 image. */
static inline int n48_rp_lin_to_g12(const n48_rp_lin_plan *p, uint32_t w, uint32_t h, const uint8_t *src, uint64_t srcBytes,
                                    uint8_t *dst, uint64_t dstBytes)
{
    n48_rp_lin_lut l;
    if (!p || !src || !dst || dstBytes != p->g12Bytes || p->srcEnd > srcBytes || !n48_rp_lin_lut_fill(p, &l)) return 0;
    for (uint64_t at = 0; at < p->g12Bytes; ) {
        uint32_t j0 = 0, n = 0; uint64_t sf = 0, sl = 0, df = 0, dl = 0;
        if (!n48_rp_lin_chunk(p, w, h, at, &j0, &n, &sf, &sl, &df, &dl) || df != at || sf + sl > srcBytes) return 0;
        if (!n48_rp_lin_band(p, &l, w, h, j0, n, src + sf, sl, dst + df, dl)) return 0;
        at += dl;
    }
    return 1;
}

/* The ask. A lin entry answers ONLY for a T# that describes exactly the image this path wrote: WIDTH and HEIGHT equal to the
 * entry's, TYPE 9 (2D), BASE_LEVEL 0 (the level held), BASE_ARRAY 0, DEPTH 0, the gfx10 SW_MODE the VRAM side said (0.0.492:
 * the one kN48RpLinModes maps to the entry's gfx12 mode, which the ask already matched), and a well-formed chain (LAST_LEVEL <=
 * MAX_MIP). Field positions are xlat12_desc.h's (gfx10 words: WIDTH w1 [31:30] | w2 [11:0] << 2, HEIGHT w2 [27:14], BASE_LEVEL
 * w3 [15:12], LAST_LEVEL w3 [19:16], SW_MODE w3 [24:20], TYPE w3 [31:28], DEPTH w4 [12:0], BASE_ARRAY w4 [28:16], MAX_MIP
 * w5 [7:4]). The element size is compared by the ask itself, as for every entry. */
static inline int n48_rp_lin_t_match(const n48_rp_ent *e, const uint32_t *t10)
{
    if (!e || !t10 || !e->lin) return 0;
    const uint32_t width = (((t10[1] >> 30) & 3u) | ((t10[2] & 0xfffu) << 2)) + 1u;
    const uint32_t height = ((t10[2] >> 14) & 0x3fffu) + 1u;
    const uint32_t baseLevel = (t10[3] >> 12) & 0xfu, lastLevel = (t10[3] >> 16) & 0xfu, sw = (t10[3] >> 20) & 0x1fu;
    const uint32_t type = t10[3] >> 28, depth = t10[4] & 0x1fffu, baseArray = (t10[4] >> 16) & 0x1fffu, maxMip = (t10[5] >> 4) & 0xfu;
    if (width != e->w || height != e->h) return 0;
    if (type != 9u || baseLevel != 0u || baseArray != 0u || depth != 0u) return 0;
    const uint32_t g10 = n48_rp_lin_g10_for_g12(e->mode);
    if (!g10 || sw != g10) return 0;
    if (lastLevel > maxMip) return 0;
    return 1;
}
/* n48_rp_ok_carry's sibling that SEES THE T# (`t10`, the raw gfx10 record) and says whether the placed copy must be clamped to
 * mip 0 (*clamp = 1: set MAX_MIP and LAST_LEVEL to 0 in OUR translated T#, never in Apple's record). For an entry any other path
 * recorded (lin 0) it is n48_rp_ok_carry exactly - same order, same counters, *clamp 0. `carry` 0 is n48_rp_ok's rule. */
static inline int n48_rp_ok_t(n48_rp *t, uint64_t ctx, uint64_t va, uint32_t mode, uint32_t askElemBytes, uint32_t arm,
                              uint32_t epoch, n48_rp_walk walk, const void *vm, uint32_t carry, uint32_t decideArm,
                              uint32_t commitArm, const uint32_t *t10, uint32_t *clamp)
{
    if (clamp) *clamp = 0u;
    t->asked++;
    if (!ctx) return 0;
    for (uint32_t k = 0; k < t->n && k < N48_RP_MAX; k++) {
        const n48_rp_ent *e = &t->e[k];
        if (e->ctx != ctx || e->va != va || e->mode != mode) continue;
        /* build 0.0.493: a known-asset entry matches its ROW (n48_rp_known_t_match), every other lin entry its image */
        if (e->lin && !(e->known ? n48_rp_known_t_match(e, t10) : n48_rp_lin_t_match(e, t10))) {
            t->linTRefused++;
            if (e->known) t->knownTRefused++;
            return 0;
        }
        if (e->elemBytes != askElemBytes) { t->elemMismatch++; return 0; }
        const int armOk = (e->arm == arm) || (carry != 0u && arm == commitArm && e->arm == decideArm);
        if (!armOk || e->epoch != epoch) { t->stale++; return 0; }
        if (!walk || !vm) { t->walkRefused++; return 0; }
        for (uint64_t p = 0; p < e->bytes; p += 4096ull) {
            uint64_t v = ~0ull;
            if (walk(vm, va + p, &v) != 1 || v != e->vram + p) { t->walkRefused++; return 0; }
        }
        t->proven++;
        if (e->lin) { t->linProven++; if (e->known) t->knownProven++; if (clamp) *clamp = 1u; }
        return 1;
    }
    return 0;
}

/* 0.0.492 PRIORITY (item 3): 1 = a gfx10 re-tile (n48_rp_retile / n48_rp_retile_kind, 46) must REFUSE this copy -
 * switch 59 is ON and the resource is a Stretch / SurfaceCopy texture (nibble 0, 1, 7 or 8) whose readable record says the
 * BACKING is linear (+0x44 [9:4] 0 or 32). Those re-tiles read the system-memory bytes as gfx10-tiled; showed they
 * are not. Switch 59 OFF: always 0 (0.0.491's re-tiles, unchanged). */
static inline int n48_rp_lin_blocks_old(uint32_t sw59, uint32_t nibble, uint32_t recOk, uint32_t swzBk)
{
    const uint32_t bkSw = (swzBk >> 4) & 0x3fu;
    return sw59 == 1u && (nibble == 0u || nibble == 1u || nibble == 7u || nibble == 8u) && recOk == 1u &&
           (bkSw == 0u || bkSw == 32u);
}
/* Page-out symmetry for the 64 KiB VRAM mode this path adds (mode 27): sticky for the boot like the other two flags, set only
 * by a switch-59 copy of a mode-27 resource. 0 while the flag is 0 (switch 59 never took one): no change. */
static inline int n48_rp_pageout_refuse_64krx(uint32_t retiled64krx, uint32_t surf)
{
    return retiled64krx && (surf & 0x1fu) == 27u;
}

/* A COMMITTED colour target of context `ctx` over [va, va + size) (the ledger's own entry and derived extent, gfx_desc_port.h
 * n48_dl_ent) drops every lin entry of that context it overlaps: the GPU may have rewritten the texture, and nothing about the
 * clamp or the bytes would then be ours. size 0 = no derived extent: the target is taken as its base only (dropped when the base
 * lies inside the entry), and a base BELOW the entry is counted in linLedNoExtent (SUSPECTED residual, not decided). Entries of
 * any other path are never touched. Returns the entries dropped. */
static inline uint32_t n48_rp_lin_drop_target(n48_rp *t, uint64_t ctx, uint64_t va, uint64_t size)
{
    uint32_t n = 0;
    if (!t) return 0u;
    for (uint32_t k = 0; k < t->n && k < N48_RP_MAX; ) {
        const n48_rp_ent *e = &t->e[k];
        if (!e->lin || e->ctx != ctx) { k++; continue; }
        const uint64_t eEnd = e->va + e->bytes;
        int hit;
        if (size) hit = (va + size < va) ? (va < eEnd) : (va < eEnd && e->va < va + size);   /* a wrapping range reaches the top */
        else {
            hit = va >= e->va && va < eEnd;
            if (!hit && va < e->va) t->linLedNoExtent++;
        }
        if (hit) { n48_rp_drop_at(t, k); t->linLedDropped++; n++; } else k++;
    }
    return n;
}

/* THE SWITCH GATE, pure so a host test drives it: 1 = the kext takes the backing-sourced copy - n48_rp_lin_check said OK AND
 * switch 59 is on (hw_resprov_lin_on(), itself requiring switch 11 and the descriptor path). Anything else: today's copy. */
static inline int n48_rp_lin_take(uint32_t why, uint32_t sw59)
{
    return why == N48_RP_LIN_OK && sw59 == 1u;
}

/* The copy-side events the kext counts (hw_resprov_lin_note). */
/* build 0.0.492: CHUNK = one streamed chunk converted (n48_rp_lin_chunk / n48_rp_lin_band); OLDBLK = a gfx10 re-tile refused
 * because the backing is linear (n48_rp_lin_blocks_old). Appended: the first four keep their values. */
enum { N48_RP_LINEV_PLANNED = 0u, N48_RP_LINEV_WRITTEN = 1u, N48_RP_LINEV_FAILED = 2u, N48_RP_LINEV_STEP0 = 3u,
       N48_RP_LINEV_CHUNK = 4u, N48_RP_LINEV_OLDBLK = 5u,
       /* build 0.0.493 (section 7): KNOWN = a known-asset conversion written and read back; KNOWNKEY = a copy that matched a
        * row's facts but not its key (not converted); KNOWNFAIL = matched facts and key but refused later (config, read, table) */
       N48_RP_LINEV_KNOWN = 6u, N48_RP_LINEV_KNOWNKEY = 7u, N48_RP_LINEV_KNOWNFAIL = 8u, N48_RP_LINEV_N = 9u };

/* STEP 0 (no behaviour change; whenever the resprov switch 11 is on, independent of switch 59): three lines per residency copy
 * of a Stretch / SurfaceCopy texture (nibble 0, 1, 7 or 8), capped per boot by the kext. Each is bounded under the kext's
 * 491-byte body cap by tests/ws_resprov_test.cpp, prefix included. Line 1: the resource and the lengths, and what switch 59's
 * check says about this copy; line 2: the record's level-0 fields; line 3: 16 dwords at the backing offset. */
#define N48_RP_LOG_BODY_CAP 491u
#define N48_RP_BK1_FMT "resprov: backing #%llu resource=%p (1/3) nibble %u fmt %#x %ux%u depth %u rowBytes %u +0xac %#x +0xad %#x " \
                       "+0xae %#x +0xc8 %#llx +0xd0 %#llx +0xe0 %s +0xf8 %#llx; dstLen %#llx srcLen %#llx mdLen %#llx " \
                       "bytes %#llx; switch-59 check %u (%s)%s"
#define N48_RP_BK1_ARGS(n, res, nib, fmt, w, h, depth, rb, ac, ad, ae, c8, d0, e0, f8, dl, sl, ml, by, why, sw) \
    (unsigned long long)(n), (const void *)(res), (unsigned)(nib), (unsigned)(fmt), (unsigned)(w), (unsigned)(h), (unsigned)(depth), \
    (unsigned)(rb), (unsigned)(ac), (unsigned)(ad), (unsigned)(ae), (unsigned long long)(c8), (unsigned long long)(d0), \
    (e0) ? "set" : "0", (unsigned long long)(f8), (unsigned long long)(dl), (unsigned long long)(sl), (unsigned long long)(ml), \
    (unsigned long long)(by), (unsigned)(why), n48_rp_lin_name(why), (sw) ? " - switch 59 ON" : " - switch 59 off, not acted on"
#define N48_RP_BK2_FMT "resprov: backing #%llu (2/3) *(res+0x180) %p%s +0x00 %u +0x02 %u +0x04 %u +0x06 %#x +0x07 %#x " \
                       "+0x08 %#llx +0x2c %#x +0x34 %u +0x36 %u +0x3a %u +0x3c %u +0x40 %#x (SW %u type %u) +0x44 %#x " \
                       "(backing SW %u type %u bit12 %u)"
#define N48_RP_BK2_ARGS(n, rec, ok, r00, r02, r04, r06, r07, r08, r2c, r34, r36, r3a, r3c, r40, r44) \
    (unsigned long long)(n), (const void *)(rec), (ok) ? "" : " UNREADABLE", (unsigned)(r00), (unsigned)(r02), (unsigned)(r04), \
    (unsigned)(r06), (unsigned)(r07), (unsigned long long)(r08), (unsigned)(r2c), (unsigned)(r34), (unsigned)(r36), \
    (unsigned)(r3a), (unsigned)(r3c), (unsigned)(r40), (unsigned)((r40) & 0x1fu), (unsigned)(((r40) >> 5) & 3u), (unsigned)(r44), \
    (unsigned)(((r44) >> 4) & 0x3fu), (unsigned)(((r44) >> 10) & 3u), (unsigned)(((r44) >> 12) & 1u)
#define N48_RP_BK3_FMT "resprov: backing #%llu (3/3) source+%#llx%s %08x %08x %08x %08x %08x %08x %08x %08x " \
                       "%08x %08x %08x %08x %08x %08x %08x %08x"
#define N48_RP_BK3_ARGS(n, off, ok, d) \
    (unsigned long long)(n), (unsigned long long)(off), (ok) ? "" : " UNREADABLE", (d)[0], (d)[1], (d)[2], (d)[3], (d)[4], (d)[5], \
    (d)[6], (d)[7], (d)[8], (d)[9], (d)[10], (d)[11], (d)[12], (d)[13], (d)[14], (d)[15]
/* build 0.0.492: a fourth step-0 line - 16 dwords of the image's MIDDLE row (y = h / 2, at the backing's own offset
 * and pitch; the element size from the resource's _ati_format_info_table row when known, else `n/a`) and the NON-ZERO bytes over
 * the whole source extent [off, off + pitch x (h - 1) + w x bpe) (`n/a` when the extent is unknown or unreadable). read only
 * the first 16 dwords, which were the avatar's transparent corner: zeros that looked like a bad pointer. */
#define N48_RP_BK4_FMT "resprov: backing #%llu (mid) row %u at source+%#llx%s %08x %08x %08x %08x %08x %08x %08x %08x " \
                       "%08x %08x %08x %08x %08x %08x %08x %08x; non-zero bytes %llu of %llu over the extent [%#llx,%#llx)%s"
#define N48_RP_BK4_ARGS(n, row, off, ok, d, nz, tot, e0, e1, cnt) \
    (unsigned long long)(n), (unsigned)(row), (unsigned long long)(off), (ok) ? "" : " UNREADABLE", (d)[0], (d)[1], (d)[2], (d)[3], \
    (d)[4], (d)[5], (d)[6], (d)[7], (d)[8], (d)[9], (d)[10], (d)[11], (d)[12], (d)[13], (d)[14], (d)[15], (unsigned long long)(nz), \
    (unsigned long long)(tot), (unsigned long long)(e0), (unsigned long long)(e1), (cnt) ? "" : " (count n/a)"
/* The middle row's source offset and the extent, for step 0 (pure: the kext reads what this says). 0 = not computable (element
 * size unknown, pitch below the width, or a wrap). */
static inline int n48_rp_lin_step0_geom(const n48_rp_lin_in *in, uint32_t *row, uint64_t *midOff, uint64_t *extFrom, uint64_t *extTo)
{
    const n48_rp_fmt_ent *f = in ? n48_rp_fmt_row(in->fmtRes) : 0;
    if (!f || !in->w || !in->h || in->pitch < in->w || in->pitch > N48_RP_MAX_DIM || in->h > N48_RP_MAX_DIM) return 0;
    const uint32_t lr = (f->d2 >> 9) & 7u;
    if (lr > 4u) return 0;
    const uint64_t pb = (uint64_t)in->pitch << lr, ext = pb * (uint64_t)(in->h - 1u) + ((uint64_t)in->w << lr);
    if (in->off > ~0ull - ext) return 0;
    *row = in->h / 2u; *midOff = in->off + pb * (uint64_t)(in->h / 2u); *extFrom = in->off; *extTo = in->off + ext;
    return 1;
}
/* switch 59's own report line (the verb and nothing else prints it) */
#define N48_RP_LIN_REPORT_FMT "p59: backing-sourced textures (`gfxneuter 59`) %s%s%s. copies planned %llu written %llu failed %llu " \
                              "step0 %llu; asks proven %llu refused no-T# %llu T#-mismatch %llu; target drops %llu (no-extent " \
                              "below %llu); clamps %llu"
/* build 0.0.492: its second line (the first is 0.0.486's, unchanged): streamed chunks converted, and the gfx10 re-tiles
 * refused because the backing is linear (n48_rp_lin_blocks_old). */
#define N48_RP_LIN_REPORT2_FMT "p59: 0.0.492 streamed chunks %llu; gfx10 re-tiles refused because the backing is LINEAR %llu"
#define N48_RP_LIN_REFUSED_TXT " *** REFUSED: a continuous arm stands ***"
#define N48_RP_LIN_INERT_TXT " - INERT: switch 11 and/or 10 off"

/* The COPIED line's re-tile suffix, NAMING THE KIND THAT RAN ('s instrument fix: the old suffix was one fixed string for
 * every kind, 256B_D included). Never longer than the old suffix, so the COPIED line cannot grow. */
#define N48_RP_RETILE_SUFFIX_OLD "; RE-TILED gfx10 ADDR_SW_4KB_D_X -> gfx12 ADDR3_4KB_2D (\xc2\xa7" "790)"
static inline const char *n48_rp_retile_suffix(uint32_t lin, uint32_t kind)
{
    if (lin) return kind == N48_RP_KIND_4KB_D_X_8 ? "; RE-TILED backing LINEAR 8bpp mip 0 -> ADDR3_4KB_2D (sw 59)"
                  : kind == N48_RP_KIND_4KB_D_X_32 ? "; RE-TILED backing LINEAR 32bpp mip 0 -> ADDR3_4KB_2D (sw 59)"
                  : "; RE-TILED backing LINEAR ? (sw 59)";
    switch (kind) {
    case N48_RP_KIND_4KB_D_X_32: return "; RE-TILED gfx10 4KB_D_X 32bpp -> gfx12 ADDR3_4KB_2D (\xc2\xa7" "790)";
    case N48_RP_KIND_4KB_D_X_8:  return "; RE-TILED gfx10 4KB_D_X 8bpp -> gfx12 ADDR3_4KB_2D (sw 46)";
    case N48_RP_KIND_4KB_D_X_64: return "; RE-TILED gfx10 4KB_D_X 64bpp -> gfx12 ADDR3_4KB_2D (sw 46)";
    case N48_RP_KIND_256B_D_8:   return "; RE-TILED gfx10 256B_D 8bpp -> gfx12 ADDR3_256B_2D (sw 46)";
    default:                     return "; RE-TILED (kind unknown)";
    }
}
/* The per-copy `shape N` names the check that ACTUALLY refused ('s second instrument fix): with switch 46 on, the 32bpp
 * check's reason was printed even when a kind hypothesis got further (the mask printed `shape 4` bpe while its 8bpp kind refused
 * BYTES_SMALL). The rank is the order n48_rp_shape_check(_kind) evaluates; the reason that got FURTHEST is the one reported. */
/* build 0.0.492: the backing-sourced copy's suffix names the gfx12 mode and element size it wrote (every string no longer
 * than the old one: tested). */
static inline const char *n48_rp_lin_suffix(uint32_t g12Mode, uint32_t bpeLog2)
{
    static const char *const k[3][5] = {
        { "; RE-TILED LINEAR 8bpp mip 0 -> ADDR3_256B_2D (sw 59)", "; RE-TILED LINEAR 16bpp mip 0 -> ADDR3_256B_2D (sw 59)",
          "; RE-TILED LINEAR 32bpp mip 0 -> ADDR3_256B_2D (sw 59)", "; RE-TILED LINEAR 64bpp mip 0 -> ADDR3_256B_2D (sw 59)",
          "; RE-TILED LINEAR 128bpp mip 0 -> ADDR3_256B_2D (sw 59)" },
        { "; RE-TILED LINEAR 8bpp mip 0 -> ADDR3_4KB_2D (sw 59)", "; RE-TILED LINEAR 16bpp mip 0 -> ADDR3_4KB_2D (sw 59)",
          "; RE-TILED LINEAR 32bpp mip 0 -> ADDR3_4KB_2D (sw 59)", "; RE-TILED LINEAR 64bpp mip 0 -> ADDR3_4KB_2D (sw 59)",
          "; RE-TILED LINEAR 128bpp mip 0 -> ADDR3_4KB_2D (sw 59)" },
        { "; RE-TILED LINEAR 8bpp mip 0 -> ADDR3_64KB_2D (sw 59)", "; RE-TILED LINEAR 16bpp mip 0 -> ADDR3_64KB_2D (sw 59)",
          "; RE-TILED LINEAR 32bpp mip 0 -> ADDR3_64KB_2D (sw 59)", "; RE-TILED LINEAR 64bpp mip 0 -> ADDR3_64KB_2D (sw 59)",
          "; RE-TILED LINEAR 128bpp mip 0 -> ADDR3_64KB_2D (sw 59)" },
    };
    return (g12Mode >= 1u && g12Mode <= 3u && bpeLog2 <= 4u) ? k[g12Mode - 1u][bpeLog2] : "; RE-TILED backing LINEAR ? (sw 59)";
}
static inline uint32_t n48_rp_shape_rank(uint32_t r)
{
    switch (r) {
    case N48_RP_SHAPE_MASK: return 1u;  case N48_RP_SHAPE_MODE: return 2u;  case N48_RP_SHAPE_TYPE: return 3u;
    case N48_RP_SHAPE_DIMS: return 4u;  case N48_RP_SHAPE_BPE: return 5u;   case N48_RP_SHAPE_DEPTH: return 6u;
    case N48_RP_SHAPE_BYTES_SMALL: case N48_RP_SHAPE_BYTES: return 7u;
    case N48_RP_SHAPE_CFG_UNREAD: return 8u; case N48_RP_SHAPE_CFG_CLASS: return 9u; case N48_RP_SHAPE_OK: return 10u;
    default: return 0u;
    }
}
static inline uint32_t n48_rp_shape_further(uint32_t a, uint32_t b)
{
    return n48_rp_shape_rank(b) > n48_rp_shape_rank(a) ? b : a;
}

/* ---- 7. build 0.0.493 - A KNOWN-ASSET ROW FOR NIBBLE-4 TILED IMAGES (switch 59, DEFAULT OFF) --------------------------------
 * SCAFFOLDING FOR THE FIRST PICTURE, not the general mechanism (that - a deferred in-place convert at the first matching ask, with a
 * copy sequence number and a per-resource page-out refusal - is later work with its own design).
 * WHY. A nibble-4 copy is a byte-for-byte memcpy of the backing (pageTextureWithMemcpy -> the NoVendBuf variant at 0xbdd6604), and
 * its resource does NOT say how the bytes are tiled: res+0x180 is 0 and res+0x1dc is the align manager's default 0x20 (mode 0), so
 * converting at copy time from the resource alone would be a guess. The mode lives only in the CONSUMER's T#. A known-asset row
 * supplies it: it names one asset by facts the copy can observe (the resource's own fields and the backing's first bytes) and
 * states the gfx10 mode its consumer uses, established from captures.
 * THE ONE ROW (every value from a capture, cited):
 *   the copy: run10g driverlog-stream.txt line 13087 `COPIED #53 resource=... type=0x40 nibble=4 bytes=0x1000 backingOffset=0 ...
 *     first source dwords 4a5462c2 aa6b7f98; GPU VA 0x400024000 ... surface SW_MODE 0 type 1 32x32 rowBytes 4096`, and the same
 *     copy in run10d (#50), run10e (#54) and run10f (#54);
 *   the KEY: the first 32 source dwords, lines 13090-13093 (`#53 source+0` .. `source+0x60`), byte-identical in all four boots;
 *   the consumer: run10d capdec/regions/F00044-USER2-4000b01a0.bin +0 `04000240 c3800000 8007c007 99600f2e 00000000 00400000 0 0`
 *     = FORMAT 56 (8_8_8_8_UNORM, 4 B), 32x32, SW_MODE 22 (ADDR_SW_4KB_D_X), TYPE 9, BASE/LAST/MAX_MIP 0, DEPTH 0 (decoded with
 *     n48_rp_lin_t_match's field positions).
 * THE KEY IS THE FIRST 32 SOURCE DWORDS, compared exactly. It is what the captures hold (the kext logs 32 dwords of every copy; no
 * capture holds the whole 4 KiB, so a whole-source hash could not be keyed from data - the brief's own fallback). 1024 bits of what
 * is visibly noise (no two of the 32 dwords equal, none zero) cannot match an unrelated resource by chance; a resource that DOES
 * carry these 128 bytes and the row's facts is this asset. The facts are checked first, so only a 32x32 0x1000-byte nibble-4
 * resource of type 0x40 at backing offset 0 is ever read for the key.
 * THE CONVERSION. gfx10 ADDR_SW_4KB_D_X at 32 bpp -> gfx12 ADDR3_4KB_2D (xlat12_desc.h's kXlat12SwModeG10ToG12 22 -> 2) through
 * n48_rp_retile, the golden-tested equations of section 1 (mip 0; one 4 KiB block: inside it the two layouts differ only by swapping
 * address bits 3 and 4). Those equations are addrlib's for the proven GB_ADDR_CONFIG class only, so Apple's gfx10 GB_ADDR_CONFIG
 * is read (the same guarded path) and must be in it (n48_rp_g10_cfg_ok), fail-closed. */
#define N48_RP_KNOWN_KEY_DW 32u
typedef struct {
    const char *name;
    uint64_t bytes, backingOffset;               /* res+0x230, res+0xf8 */
    uint32_t nibble, resType, w, h, rowBytes;    /* (res+0x1b4 >> 26) & 0xf, res+0x14 (u8), res+0xb0 / +0xb2 (u16), res+0xb8 */
    uint32_t g10Sw, fmt10, elemBytes, g12Mode;   /* the consumer T#'s SW_MODE and FORMAT, its element size, the gfx12 mode written */
    uint32_t key[N48_RP_KNOWN_KEY_DW];           /* the backing's first 32 dwords */
} n48_rp_known_row;
static const n48_rp_known_row kN48RpKnownNib4[] = {
    { "AZ 32x32 noise (F35 unit 12)", 0x1000ull, 0ull, 4u, 0x40u, 32u, 32u, 4096u, 22u, 56u, 4u, 2u,
      { 0x4a5462c2u, 0xaa6b7f98u, 0xa77a81afu, 0xb2937193u, 0xb9798677u, 0xc7af474fu, 0x1c377805u, 0x7842809au,
        0x7a7b2a9au, 0x3996a78eu, 0xaacb6fc3u, 0x8a984fb0u, 0xc3bf9452u, 0x0740bcbdu, 0xa17dcfaau, 0x5e5b8632u,
        0x80c2b981u, 0x80c194ddu, 0x93304498u, 0x75b46da5u, 0x8583b86bu, 0xce8deb45u, 0x94423421u, 0x2eef509eu,
        0x606b6f3cu, 0x112848b3u, 0x6a8785b1u, 0x94addb6fu, 0xd7b0668eu, 0xb2d258edu, 0x858d7479u, 0xa65a8b28u } },
};
#define N48_RP_KNOWN_N 1u
#define N48_RP_KNOWN_NONE 0xffffffffu
enum { N48_RP_KN_OK = 0, N48_RP_KN_FACTS = 1, N48_RP_KN_CFG = 2, N48_RP_KN_READ = 3, N48_RP_KN_KEY = 4, N48_RP_KN_CONVERT = 5,
       N48_RP_KN_TABLE = 6, N48_RP_KN_ALLOC = 7, N48_RP_KN_REASONS = 8 };
static inline const char *n48_rp_known_why(uint32_t r)
{
    static const char *const k[N48_RP_KN_REASONS] = {
        "ok", "facts differ", "gfx10 GB_ADDR_CONFIG unread or outside the proven class", "the backing read short",
        "facts match but the key does not", "the row is not a convertible shape", "the page-out table is full", "allocation"
    };
    return r < N48_RP_KN_REASONS ? k[r] : "?";
}
/* What the kext reads from the resource before anything else (Navi48AccelPeer.cpp rp_known_prepare). */
typedef struct { uint32_t nibble, resType, w, h, rowBytes; uint64_t bytes, backingOffset; } n48_rp_known_in;

/* 7a. FACTS: the row whose facts are exactly the copy's, or N48_RP_KNOWN_NONE. Nothing is read for a copy with no row. */
static inline uint32_t n48_rp_known_row_for(const n48_rp_known_in *in)
{
    if (!in) return N48_RP_KNOWN_NONE;
    for (uint32_t i = 0; i < N48_RP_KNOWN_N; i++) {
        const n48_rp_known_row *r = &kN48RpKnownNib4[i];
        if (in->nibble == r->nibble && in->resType == r->resType && in->bytes == r->bytes && in->backingOffset == r->backingOffset &&
            in->w == r->w && in->h == r->h && in->rowBytes == r->rowBytes) return i;
    }
    return N48_RP_KNOWN_NONE;
}
/* 7b. KEY: 1 = the source's first 32 dwords are the row's (srcBytes of `src` were read). */
static inline int n48_rp_known_key_ok(uint32_t row, const uint32_t *src, uint64_t srcBytes)
{
    if (row >= N48_RP_KNOWN_N || !src || srcBytes < 4ull * N48_RP_KNOWN_KEY_DW) return 0;
    for (uint32_t i = 0; i < N48_RP_KNOWN_KEY_DW; i++) if (src[i] != kN48RpKnownNib4[row].key[i]) return 0;
    return 1;
}
/* 7c. CONVERT: the whole allocation, gfx10 ADDR_SW_4KB_D_X 32 bpp -> gfx12 ADDR3_4KB_2D (n48_rp_retile, direction 1). Only a row of
 * that one shape is convertible (the equations exist for nothing else here), and only under a proven-class GB_ADDR_CONFIG. 1 or
 * N48_RP_KN_CFG / N48_RP_KN_CONVERT (nothing written). */
static inline uint32_t n48_rp_known_convert(uint32_t row, uint32_t gbRead, uint32_t gb, const uint32_t *src, uint32_t *dst,
                                            uint64_t bytes)
{
    if (row >= N48_RP_KNOWN_N) return N48_RP_KN_CONVERT;
    const n48_rp_known_row *r = &kN48RpKnownNib4[row];
    if (gbRead != 1u || !n48_rp_g10_cfg_ok(gb)) return N48_RP_KN_CFG;
    if (r->g10Sw != N48_RP_G10_4KB_D_X || r->elemBytes != 4u || r->g12Mode != N48_RP_G12_4KB_2D || bytes != r->bytes ||
        bytes != n48_rp_bytes(r->w, r->h)) return N48_RP_KN_CONVERT;
    return n48_rp_retile(src, dst, r->w, r->h, bytes, 1) ? N48_RP_KN_OK : N48_RP_KN_CONVERT;
}
/* 7d. RECORD: a known-asset copy is exactly its row's (n48_rp_record's clause). */
static inline int n48_rp_known_copy_ok(const n48_rp_copy *c)
{
    if (!c || c->lin < N48_RP_LIN_KNOWN0 || c->lin - N48_RP_LIN_KNOWN0 >= N48_RP_KNOWN_N) return 0;
    const n48_rp_known_row *r = &kN48RpKnownNib4[c->lin - N48_RP_LIN_KNOWN0];
    return c->mode == r->g12Mode && c->w == r->w && c->h == r->h && c->elemBytes == r->elemBytes && c->bytes == r->bytes &&
           c->retiled == 1u;
}
/* 7e. THE ASK: n48_rp_lin_t_match's checks (dims equal to the entry's, TYPE 9, BASE_LEVEL 0, BASE_ARRAY 0, DEPTH 0, LAST_LEVEL <=
 * MAX_MIP) against the ROW, plus the row's gfx10 SW_MODE and FORMAT - the mode is the one fact the resource could not supply, so
 * the T# must state exactly the mode the row was established from (a T# of SW 21 or 23 maps to the same gfx12 mode and is still
 * refused). The element size is compared by the ask itself (the entry's elemBytes is the row's). */
static inline int n48_rp_known_t_match(const n48_rp_ent *e, const uint32_t *t10)
{
    if (!e || !t10 || !e->lin || !e->known || e->known > N48_RP_KNOWN_N) return 0;
    const n48_rp_known_row *r = &kN48RpKnownNib4[e->known - 1u];
    const uint32_t width = (((t10[1] >> 30) & 3u) | ((t10[2] & 0xfffu) << 2)) + 1u;
    const uint32_t height = ((t10[2] >> 14) & 0x3fffu) + 1u;
    const uint32_t baseLevel = (t10[3] >> 12) & 0xfu, lastLevel = (t10[3] >> 16) & 0xfu, sw = (t10[3] >> 20) & 0x1fu;
    const uint32_t type = t10[3] >> 28, depth = t10[4] & 0x1fffu, baseArray = (t10[4] >> 16) & 0x1fffu, maxMip = (t10[5] >> 4) & 0xfu;
    const uint32_t fmt = (t10[1] >> 20) & 0x1ffu;
    if (e->w != r->w || e->h != r->h || e->mode != r->g12Mode || width != r->w || height != r->h) return 0;
    if (type != 9u || baseLevel != 0u || baseArray != 0u || depth != 0u) return 0;
    if (sw != r->g10Sw) return 0;
    if (fmt != r->fmt10) return 0;
    if (lastLevel > maxMip) return 0;
    return 1;
}
/* 7f. PAGE-OUT: REFUSED for every resource a known-asset conversion was prepared for (sticky for the boot). The existing per-mode
 * flags cannot see these resources (their swizzle dword says mode 0), so the resource itself is remembered, in a fixed table,
 * BEFORE the first byte is written; a full table refuses the conversion (N48_RP_KN_TABLE), so no converted resource is ever
 * unremembered. Lock-free: a slot goes 0 -> resource once (CAS) and never changes. 1 = present or added, 0 = full. */
static inline int n48_rp_known_res_add(uint64_t *tab, uint32_t cap, uint64_t res)
{
    if (!tab || !res) return 0;
    for (uint32_t i = 0; i < cap; i++) if (__atomic_load_n(&tab[i], __ATOMIC_ACQUIRE) == res) return 1;
    for (uint32_t i = 0; i < cap; i++) {
        uint64_t exp = 0ull;
        if (__atomic_compare_exchange_n(&tab[i], &exp, res, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return 1;
        if (exp == res) return 1;
    }
    return 0;
}
static inline int n48_rp_known_pageout(const uint64_t *tab, uint32_t cap, uint64_t res)
{
    if (!tab || !res) return 0;
    for (uint32_t i = 0; i < cap; i++) if (__atomic_load_n(&tab[i], __ATOMIC_ACQUIRE) == res) return 1;
    return 0;
}
/* The COPIED line's suffix for a known-asset conversion (no longer than N48_RP_RETILE_SUFFIX_OLD: tested). */
#define N48_RP_KNOWN_SUFFIX "; RE-TILED KNOWN nib4 4KB_D_X 32bpp -> ADDR3_4KB_2D (sw 59)"
/* One line per converted asset, naming the row (written and read back; the ledger's verdict follows on the resprov line). */
#define N48_RP_KNOWN_FMT "resprov: KNOWN ASSET row %u \"%s\" resource=%p: gfx10 SW %u FORMAT %u %ux%u -> gfx12 mode %u (ADDR3_4KB_2D), " \
                         "%#llx bytes written, %llu dword(s) read back, %llu MISMATCHED (sw 59, 0.0.493)"
#define N48_RP_KNOWN_ARGS(row, res, cmp, mis) \
    (unsigned)(row), kN48RpKnownNib4[(row) < N48_RP_KNOWN_N ? (row) : 0u].name, (const void *)(res), \
    (unsigned)kN48RpKnownNib4[(row) < N48_RP_KNOWN_N ? (row) : 0u].g10Sw, (unsigned)kN48RpKnownNib4[(row) < N48_RP_KNOWN_N ? (row) : 0u].fmt10, \
    (unsigned)kN48RpKnownNib4[(row) < N48_RP_KNOWN_N ? (row) : 0u].w, (unsigned)kN48RpKnownNib4[(row) < N48_RP_KNOWN_N ? (row) : 0u].h, \
    (unsigned)kN48RpKnownNib4[(row) < N48_RP_KNOWN_N ? (row) : 0u].g12Mode, \
    (unsigned long long)kN48RpKnownNib4[(row) < N48_RP_KNOWN_N ? (row) : 0u].bytes, (unsigned long long)(cmp), (unsigned long long)(mis)
/* A copy whose facts match a row but whose key or a later step does not: logged once per row and reason. */
#define N48_RP_KNOWN_REFUSED_FMT "resprov: KNOWN ASSET row %u \"%s\" resource=%p NOT converted: %s (reason %u; logged once per row and " \
                                 "reason) - the plain byte-for-byte copy stands, unrecorded"
/* switch 59's third report line */
#define N48_RP_LIN_REPORT3_FMT "p59: 0.0.493 known assets (%u row(s)) converted %llu; facts matched but key differed %llu; refused after " \
                               "the key %llu; asks proven %llu, T# refused %llu"

#endif /* N48_WS_RESPROV_H */
