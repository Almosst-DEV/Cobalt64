// gfx_reloc.h — VERTEX RELOCATION: the arena bookkeeping. Pure C, host-tested by tests/gfx_reloc_test.cpp; the kext compiles
// the SAME header (; the design is XLAT12_RELOC_* in src/xlat12/xlat12_ib.h, 0.0.265 / ).
//
// WHY RELOCATION EXISTS. Substituting a shader means writing our gfx1201 program over Apple's, in place, inside the allocation
// Apple made. For fragment stages that works: put our hand-written Const_PS in 44 of Apple's 48 bytes. For VERTEX stages
// on gfx12 it often cannot, because gfx12 has no legacy VS stage - every vertex shader is a merged NGG shader carrying a
// prologue that sets exec, takes the lane index, reads the merged wave counts and exports the primitive. That prologue ALONE
// measures 92 bytes (tools/build-cache-shader.sh), which is most of a small Apple program. RELOCATION PUTS THE PROGRAM
// SOMEWHERE ELSE AND POINTS THE HARDWARE AT IT, instead of trying to shrink it.
//
// *** CORRECTION, 0.0.313. THIS HEADER USED TO SAY "Apple's RectPosTexFast_VS slot is 128 bytes with ZERO
// SLACK, so no gfx1201 vertex shader whatsoever fits it". THAT WAS A MISREAD and it is withdrawn. The 128 was the gap in
// the DYLIB ON DISK to the next symbol (gShRegisters_...), which is PM4 register data consumed on the CPU, not shader code
// in GPU memory. In GPU memory the floor is 256 bytes, because SPI_SHADER_PGM_LO holds VA >> 8 and every program must
// therefore be 256-aligned - the cache's own record for this key says capacity 0x100 = 256, and a scan of every program
// address gfxcap1 names (24 distinct VAs, all 256-aligned) measures the smallest gap between consecutive programs at 0x200
// = 512 bytes, with ~32 KiB clear either side of this one. A 200-byte substitute therefore FITS.
// Relocation remains built, tested and the safer mechanism for a program that genuinely does not fit - but "128 bytes with
// zero slack" must not be quoted as the reason for it again. ***
//
// WHAT IS ALREADY BUILT, and what this header adds. The IB side of relocation EXISTS and is host-tested; 0.0.399
// withdraws the older "and has run on hardware": ran the KEXT half's PLACEMENT and the CPU-side MM read-back on
// hardware, and record that no relocated program has ever been FETCHED or executed. The translator takes
// `xlat12_draw_extra.vs_pgm_va` and, at 0xb120 (SPI_SHADER_PGM_LO_VS), emits gfx12's 0xb224 (PGM_LO_ES) holding OUR address
// >> 8 instead of re-homing Apple's, and the same translator already moves the whole legacy-VS stream onto the NGG path
// (USER_DATA_VS_0..11 -> USER_DATA_GS_0..11 at +0x100, VGT_SHADER_STAGES_EN -> 0x28a98 with a synthesised value,
// SPI_SHADER_IDX_FORMAT overridden for the prim export, and d_synth() emitting the NGG state a legacy-VS stream never
// programs). What was missing - the project notes "vertex relocation's kext half is not built" - is the part below the
// translator: WHERE the program goes, HOW it gets there, and WHO may claim a piece of it.
//
// WHERE THE ROOM COMES FROM, and why it is that and not something else. Three candidates were considered:
//   - a fresh mapping of our own VRAM into the client's page table: needs writing leaf PTEs into APPLE'S tables for VMID 2.
//     Possible (the walker exists) but it mutates Apple's page tables on every frame's behalf. Rejected.
//   - slack inside an existing Apple resource: no page-table work at all, but the slack is not ours, its extent is not
//     recorded anywhere, and a resource can be re-uploaded under us at any residency copy. Rejected as a base for code the
//     hardware will fetch.
//   - THE RING REGION WE ALREADY OWN. `ringmap` carves a VRAM region and maps 169 64 KiB leaves through our OWN L1 block at
//     root slot 511, with ringmap_leaf() setting EXECUTABLE | READABLE | WRITEABLE. Apple's tables point at none of it; we
//     allocated it, we wrote the page tables, and the geometry engine already fetches its rings from it. The tail above the
//     ring-offsets descriptor page, [+0xA81000, +0xA8F000) = 56 KiB since 0.0.413 (it was 60 KiB before the last 4 KiB,
//     [+0xA8F000, +0xA90000), became the fence828 owned slot page — notes/design/FENCE-OWNED-SLOT.md), is reserved for
//     exactly this (XLAT12_RELOC_ARENA_OFF/BYTES). CHOSEN.
// LIFETIME. The arena lives and dies with the ringmap carve: it is valid only while `built` and it is emptied whenever the
// region is rebuilt. 0.0.399: a placement records the VA base it was made under and a MOVE of that base empties
// the arena too, because an offset from the old mapping would name a different VA under the new one; `n48_reloc_find` also
// refuses a row whose base does not match the base it is being dereferenced through. A placement is additionally keyed by the
// SHADER-CACHE KEY of the program it holds, so a different blob - a different program for the same shader - cannot silently
// reuse a placement made for the old one; the caller passes the shader-cache epoch and a change empties the arena. Nothing
// here frees a placement: the arena is append-only within a carve, which is what makes a VA we handed to the hardware safe
// to keep using.
#ifndef N48_GFX_RELOC_H
#define N48_GFX_RELOC_H

#include <stdint.h>
#include "xlat12_ib.h"    /* XLAT12_RELOC_ARENA_OFF / _BYTES / _ALIGN: one definition, shared with the translator */

enum {
    N48_RELOC_OK = 0,
    N48_RELOC_E_ARG,        /* no arena, key 0, or zero bytes */
    N48_RELOC_E_NOT_BUILT,  /* the ring region is not carved and mapped */
    N48_RELOC_E_FULL,       /* the arena has no room left for this program */
    N48_RELOC_E_ROWS,       /* the placement table is full */
    N48_RELOC_E_RESIZED,    /* this key is already placed at a DIFFERENT size: never move a live program */
    N48_RELOC_E_ALIGN,      /* the resulting VA is not XLAT12_RELOC_ALIGN aligned, or is >= 2^48 */
    N48_RELOC_REASONS
};

static inline const char *n48_reloc_reason(uint32_t r)
{
    static const char *const n[N48_RELOC_REASONS] = { "ok", "bad-argument", "ring-region-not-built", "arena-full",
                                                      "placement-table-full", "key-placed-at-another-size", "bad-alignment" };
    return r < N48_RELOC_REASONS ? n[r] : "?";
}

#define N48_RELOC_ROWS 16u

/* 0.0.313: `id` is the xlat12 identity-table index of the image PLACED here, or -1 when we hold none.
 * It is recorded at upload, where the bytes are already in hand, because the draw resolver cannot recover it later: the
 * resolver identifies a program by matching the bytes at APPLE'S address, and a relocated program's bytes there stay
 * Apple's for ever. Without this the row for our own image could never be found and the guard it carries - "this image
 * exports no parameters" - would never be applied. */
typedef struct { uint64_t key; uint32_t off, bytes, stage, writes; int32_t id;
                 /* 0.0.399: the ring region's VA base this placement was made under. A row is only ever
                  * dereferenced through the base it was placed at, so a carve that moves `gRingMap.vaBase` cannot turn
                  * its offset into a VA that now names something else. */
                 uint64_t vaBase; } n48_reloc_row;

typedef struct {
    n48_reloc_row row[N48_RELOC_ROWS];
    uint32_t used;          /* rows in use */
    uint32_t cursor;        /* the next free byte, relative to XLAT12_RELOC_ARENA_OFF */
    uint64_t epoch;         /* the shader-cache epoch the placements were made under */
    uint64_t carve;         /* the ringmap carve generation the placements belong to */
    uint64_t vaBase;        /* 0.0.399: the VA base these placements belong to; a move empties the arena */
    uint64_t places, reuses, refused[N48_RELOC_REASONS], resets;
} n48_reloc_arena;

/* Empty the arena. Called when the ring region is (re)built or the shader-cache epoch moves. */
static inline void n48_reloc_reset(n48_reloc_arena *a, uint64_t carve, uint64_t epoch)
{
    for (uint32_t i = 0; i < N48_RELOC_ROWS; i++) { a->row[i].key = 0; a->row[i].bytes = 0; a->row[i].vaBase = 0; }
    a->used = 0;
    a->cursor = 0;
    a->carve = carve;
    a->epoch = epoch;
    a->vaBase = 0;
    a->resets++;
}

static inline uint32_t n48_reloc_round(uint32_t n)
{
    return (n + (XLAT12_RELOC_ALIGN - 1u)) & ~(XLAT12_RELOC_ALIGN - 1u);
}

/* The GPU VA of a placement, from the ring region's VA base. */
static inline uint64_t n48_reloc_va(uint64_t ring_va_base, uint32_t off)
{
    return ring_va_base + (uint64_t)XLAT12_RELOC_ARENA_OFF + (uint64_t)off;
}

/* Claim `bytes` for `key`. Returns N48_RELOC_OK and sets *off (and *fresh to 1 when the caller must now WRITE the program
 * there, 0 when this key was already placed and the bytes are already in VRAM). Placement is stable: the same key always
 * gets the same offset within a carve, so a VA already handed to the hardware never moves.
 * 0.0.399: the placement also records the ring region's VA base it was made under, and a base that has MOVED
 * empties the arena exactly as a new carve or epoch does - an offset from the old mapping names a different VA now. */
static inline uint32_t n48_reloc_place(n48_reloc_arena *a, uint64_t ring_va_base, uint32_t built, uint64_t carve,
                                       uint64_t epoch, uint64_t key, uint32_t bytes, uint32_t stage,
                                       uint32_t *off, uint32_t *fresh)
{
    uint32_t need;
    if (off) *off = 0;
    if (fresh) *fresh = 0;
    if (!a || !off || !fresh) return N48_RELOC_E_ARG;
    if (!built) { a->refused[N48_RELOC_E_NOT_BUILT]++; return N48_RELOC_E_NOT_BUILT; }
    if (!key || !bytes) { a->refused[N48_RELOC_E_ARG]++; return N48_RELOC_E_ARG; }
    if (carve != a->carve || epoch != a->epoch || ring_va_base != a->vaBase) n48_reloc_reset(a, carve, epoch);
    a->vaBase = ring_va_base;
    for (uint32_t i = 0; i < a->used; i++)
        if (a->row[i].key == key) {
            if (a->row[i].bytes != bytes) { a->refused[N48_RELOC_E_RESIZED]++; return N48_RELOC_E_RESIZED; }
            *off = a->row[i].off;
            a->reuses++;
            return N48_RELOC_OK;
        }
    if (a->used >= N48_RELOC_ROWS) { a->refused[N48_RELOC_E_ROWS]++; return N48_RELOC_E_ROWS; }
    need = n48_reloc_round(bytes);
    if (need > XLAT12_RELOC_ARENA_BYTES || a->cursor > XLAT12_RELOC_ARENA_BYTES - need) {
        a->refused[N48_RELOC_E_FULL]++;
        return N48_RELOC_E_FULL;
    }
    {
        const uint64_t va = n48_reloc_va(ring_va_base, a->cursor);
        if ((va & (XLAT12_RELOC_ALIGN - 1u)) != 0u || va >= (1ull << 48)) {
            a->refused[N48_RELOC_E_ALIGN]++;
            return N48_RELOC_E_ALIGN;
        }
    }
    {
        n48_reloc_row *r = &a->row[a->used++];
        r->key = key; r->off = a->cursor; r->bytes = bytes; r->stage = stage; r->writes = 0; r->id = -1;
        r->vaBase = ring_va_base;
    }
    *off = a->cursor;
    *fresh = 1;
    a->cursor += need;
    a->places++;
    return N48_RELOC_OK;
}

/* The offset a key sits at, or 0 with a 0 return when it is not placed AT `ring_va_base`. 0.0.399: the base is
 * part of the question - a row placed under a mapping that has since moved must NOT be dereferenced through the current
 * one, so `n48_reloc_va(ring_va_base, *off)` can never name bytes we did not write. */
static inline uint32_t n48_reloc_find(const n48_reloc_arena *a, uint64_t key, uint64_t ring_va_base, uint32_t *off)
{
    if (off) *off = 0;
    if (!a || !key || !ring_va_base) return 0;
    for (uint32_t i = 0; i < a->used; i++)
        if (a->row[i].key == key) {
            if (a->row[i].vaBase != ring_va_base) return 0;
            if (off) *off = a->row[i].off;
            return 1;
        }
    return 0;
}

/* The identity of the image placed for `key`: >= 0 an xlat12 identity index, -1 none placed or none known. */
static inline int32_t n48_reloc_id(const n48_reloc_arena *a, uint64_t key)
{
    if (!a || !key) return -1;
    for (uint32_t i = 0; i < a->used; i++)
        if (a->row[i].key == key) return a->row[i].id;
    return -1;
}

/* Record that identity, at upload. Refuses to change one already set to something else: a placement's identity is a
 * property of the bytes in the arena, and two different answers for one key would mean the arena was written twice. */
static inline uint32_t n48_reloc_set_id(n48_reloc_arena *a, uint64_t key, int32_t id)
{
    if (!a || !key) return 0;
    for (uint32_t i = 0; i < a->used; i++)
        if (a->row[i].key == key) {
            if (a->row[i].id >= 0 && a->row[i].id != id) return 0;
            a->row[i].id = id;
            return 1;
        }
    return 0;
}

/* Bytes still claimable (after rounding the cursor). */
static inline uint32_t n48_reloc_free(const n48_reloc_arena *a)
{
    return a && a->cursor <= XLAT12_RELOC_ARENA_BYTES ? XLAT12_RELOC_ARENA_BYTES - a->cursor : 0u;
}

#endif
