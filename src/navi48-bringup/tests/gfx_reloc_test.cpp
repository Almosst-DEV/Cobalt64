// gfx_reloc_test.cpp — the vertex-relocation arena: placement, stability, the refusals, the VA-base staleness (0.0.399,
//) and a PLANTED-DEFECT CONTROL of eight mutants, each of which must break at least one named check.
//     clang++ -std=c++17 -Wall -Wextra -Werror -O1 -fsanitize=address,undefined -fno-sanitize-recover=all \
//         -I src/navi48-bringup/src/apple -I src/xlat12 src/navi48-bringup/tests/gfx_reloc_test.cpp -o /tmp/rltest && /tmp/rltest
#include <cstdio>
#include <cstdint>
#include <cstring>
#include "gfx_reloc.h"

static int gFail = 0, gRun = 0, gQuiet = 0;
static void expect_u(const char *what, uint64_t got, uint64_t want)
{
    gRun++;
    if (got != want) { gFail++; if (!gQuiet) std::printf("FAIL  %-72s got %#llx want %#llx\n", what,
                                                         (unsigned long long)got, (unsigned long long)want); }
    else if (!gQuiet) std::printf("ok    %-72s %#llx\n", what, (unsigned long long)got);
}

typedef uint32_t (*PlaceFn)(n48_reloc_arena *, uint64_t, uint32_t, uint64_t, uint64_t, uint64_t, uint32_t, uint32_t,
                            uint32_t *, uint32_t *);
static PlaceFn gPlace = &n48_reloc_place;
typedef uint32_t (*FindFn)(const n48_reloc_arena *, uint64_t, uint64_t, uint32_t *);
static FindFn gFind = &n48_reloc_find;

// The ring region's VA base as ringmap builds it: root slot 511 of CONTEXT2's window (AppleHardwareHook.cpp,).
static const uint64_t kRingVa = 0x23F0000000ull;
// 0.0.399: a DIFFERENT base the same carve could move to. The arena's VA base is not stable across a re-carve.
static const uint64_t kRingVa2 = 0x2400000000ull;
// Real shader-cache keys of vertex programs (tools/m4-xlat/sa-programs.tsv, read from Apple's own records).
static const uint64_t kVkba6 = 0xe52b4557b76ddb3eull;   // vkba6pa7jbebia3, Apple 1312 B in a 1792 B allocation
static const uint64_t kPlane = 0x3f629a1c50bce538ull;   // plane__vertex,   our gfx1201 is 224 B
static const uint64_t kRect  = 0xe3619022d1d4f224ull;   // gShaderCode_gfx10_RectPosTexFast_VS: 200 B of ours. (The "128 B
                                                        // slot" this line used to cite was the DYLIB gap, not GPU memory:
                                                        // measured the real floor at 256 and the gap at 512.)

// ---- mutants ------------------------------------------------------------------------------------------------------------
// M1: a second placement of the same key gets a NEW offset — a live program would move under the hardware.
static uint32_t m1_place(n48_reloc_arena *a, uint64_t va, uint32_t built, uint64_t carve, uint64_t epoch, uint64_t key,
                         uint32_t bytes, uint32_t stage, uint32_t *off, uint32_t *fresh)
{
    if (a && key) for (uint32_t i = 0; i < a->used; i++) if (a->row[i].key == key) a->row[i].key = 0;
    return n48_reloc_place(a, va, built, carve, epoch, key, bytes, stage, off, fresh);
}
// M2: the cursor is not rounded, so the next program starts unaligned and PGM_LO (VA >> 8) loses its low bits.
static uint32_t m2_place(n48_reloc_arena *a, uint64_t va, uint32_t built, uint64_t carve, uint64_t epoch, uint64_t key,
                         uint32_t bytes, uint32_t stage, uint32_t *off, uint32_t *fresh)
{
    const uint32_t before = a ? a->cursor : 0u;
    const uint32_t st = n48_reloc_place(a, va, built, carve, epoch, key, bytes, stage, off, fresh);
    if (a && st == N48_RELOC_OK && fresh && *fresh) a->cursor = before + bytes;   /* unrounded */
    return st;
}
// M3: the arena bound is not checked, so a placement runs past the 60 KiB region into the L1 block above it.
static uint32_t m3_place(n48_reloc_arena *a, uint64_t va, uint32_t built, uint64_t carve, uint64_t epoch, uint64_t key,
                         uint32_t bytes, uint32_t stage, uint32_t *off, uint32_t *fresh)
{
    uint32_t st = n48_reloc_place(a, va, built, carve, epoch, key, bytes, stage, off, fresh);
    if (st == N48_RELOC_E_FULL && a && a->used < N48_RELOC_ROWS) {
        n48_reloc_row *r = &a->row[a->used++];
        r->key = key; r->off = a->cursor; r->bytes = bytes; r->stage = stage; r->writes = 0;
        r->vaBase = va;
        *off = a->cursor; *fresh = 1; a->cursor += n48_reloc_round(bytes);
        st = N48_RELOC_OK;
    }
    return st;
}
// M4: an unbuilt ring region is accepted, so the VA names memory nothing maps.
static uint32_t m4_place(n48_reloc_arena *a, uint64_t va, uint32_t built, uint64_t carve, uint64_t epoch, uint64_t key,
                         uint32_t bytes, uint32_t stage, uint32_t *off, uint32_t *fresh)
{
    (void)built;
    return n48_reloc_place(a, va, 1u, carve, epoch, key, bytes, stage, off, fresh);
}
// M5: a key placed at one size is silently re-placed at another, so the hardware fetches a program of the wrong length.
static uint32_t m5_place(n48_reloc_arena *a, uint64_t va, uint32_t built, uint64_t carve, uint64_t epoch, uint64_t key,
                         uint32_t bytes, uint32_t stage, uint32_t *off, uint32_t *fresh)
{
    if (a) for (uint32_t i = 0; i < a->used; i++) if (a->row[i].key == key) a->row[i].bytes = bytes;
    return n48_reloc_place(a, va, built, carve, epoch, key, bytes, stage, off, fresh);
}
// M6: a change of carve or epoch does not empty the arena, so offsets from a dead mapping are handed out again.
static uint32_t m6_place(n48_reloc_arena *a, uint64_t va, uint32_t built, uint64_t carve, uint64_t epoch, uint64_t key,
                         uint32_t bytes, uint32_t stage, uint32_t *off, uint32_t *fresh)
{
    if (a) { a->carve = carve; a->epoch = epoch; }
    return n48_reloc_place(a, va, built, carve, epoch, key, bytes, stage, off, fresh);
}
// 0.0.399. M7: find ignores the VA base it is asked through - 0.0.398's lookup, which would hand a moved row's
// offset to the caller and point the hardware at whatever now lives at that VA.
static uint32_t m7_find(const n48_reloc_arena *a, uint64_t key, uint64_t va, uint32_t *off)
{
    (void)va;
    if (off) *off = 0;
    if (!a || !key) return 0;
    for (uint32_t i = 0; i < a->used; i++)
        if (a->row[i].key == key) { if (off) *off = a->row[i].off; return 1; }
    return 0;
}
// M8: a moved VA base does NOT empty the arena, so rows from the old mapping survive and a new placement can start above them.
static uint32_t m8_place(n48_reloc_arena *a, uint64_t va, uint32_t built, uint64_t carve, uint64_t epoch, uint64_t key,
                         uint32_t bytes, uint32_t stage, uint32_t *off, uint32_t *fresh)
{
    if (a) { a->carve = carve; a->epoch = epoch; a->vaBase = va; }
    return n48_reloc_place(a, va, built, carve, epoch, key, bytes, stage, off, fresh);
}

// 0.0.399: A PLACEMENT BELONGS TO THE VA BASE IT WAS MADE UNDER. A row is only ever dereferenced through that
// base; a moved mapping must find nothing (never a stale offset turned into an address), and a re-place under the new
// base must empty the arena exactly as a new carve does. Both directions are checked, plus the base-0 refusal.
static void checks_base()
{
    static n48_reloc_arena a;
    std::memset(&a, 0, sizeof a);
    uint32_t off = 0, fresh = 0;

    expect_u("F2 place kPlane at base A", gPlace(&a, kRingVa, 1, 1, 1, kPlane, 224, 1, &off, &fresh), N48_RELOC_OK);
    expect_u("  it lands at 0", off, 0);
    expect_u("  the arena records base A", a.vaBase, kRingVa);
    expect_u("  the row records base A", a.row[0].vaBase, kRingVa);

    expect_u("F2 find at the PLACED base proves", gFind(&a, kPlane, kRingVa, &off), 1);
    off = 0xdead;
    expect_u("F2 find at a MOVED base REFUSES (a stale offset is never dereferenced)", gFind(&a, kPlane, kRingVa2, &off), 0);
    expect_u("  and no offset is handed out", off, 0);

    expect_u("F2 re-placing under a moved base is OK", gPlace(&a, kRingVa2, 1, 1, 1, kRect, 200, 1, &off, &fresh), N48_RELOC_OK);
    expect_u("  the arena is EMPTIED (one row)", a.used, 1);
    expect_u("  the new placement starts at 0", off, 0);
    expect_u("  the arena now records base B", a.vaBase, kRingVa2);
    expect_u("  the OLD base no longer finds the old key", gFind(&a, kPlane, kRingVa, &off), 0);
    expect_u("  the NEW base finds the new key", gFind(&a, kRect, kRingVa2, &off), 1);
    expect_u("F2 find of the new row at the OLD base refuses too", gFind(&a, kRect, kRingVa, &off), 0);
    expect_u("F2 a find at base 0 is no base at all and refuses", gFind(&a, kRect, 0ull, &off), 0);
}

static void checks()
{
    static n48_reloc_arena a;
    std::memset(&a, 0, sizeof a);
    uint32_t off = 0, fresh = 0;
    // an unbuilt ring region refuses before anything else
    expect_u("an unbuilt ring region refuses", gPlace(&a, kRingVa, 0, 1, 1, kPlane, 224, 1, &off, &fresh),
             N48_RELOC_E_NOT_BUILT);
    expect_u("  nothing placed", a.used, 0);

    // the first placement
    expect_u("first placement ok", gPlace(&a, kRingVa, 1, 1, 1, kPlane, 224, 1, &off, &fresh), N48_RELOC_OK);
    expect_u("  at offset 0", off, 0);
    expect_u("  and the caller must write it", fresh, 1);
    const uint64_t va0 = n48_reloc_va(kRingVa, off);
    expect_u("  its VA is inside the arena", va0, kRingVa + XLAT12_RELOC_ARENA_OFF);
    expect_u("  and 256-byte aligned, because PGM_LO holds VA >> 8", va0 & (XLAT12_RELOC_ALIGN - 1u), 0);

    // the second lands on the next 256-byte boundary, not at 224
    expect_u("second placement ok", gPlace(&a, kRingVa, 1, 1, 1, kRect, 200, 1, &off, &fresh), N48_RELOC_OK);
    expect_u("  rounded up past the first (224 -> 256)", off, XLAT12_RELOC_ALIGN);
    expect_u("  its VA is aligned too", n48_reloc_va(kRingVa, off) & (XLAT12_RELOC_ALIGN - 1u), 0);
    expect_u("  two rows", a.used, 2);

    // STABILITY: a program already placed never moves, and the caller is told not to write it again
    expect_u("re-placing the first key is ok", gPlace(&a, kRingVa, 1, 1, 1, kPlane, 224, 1, &off, &fresh), N48_RELOC_OK);
    expect_u("  at the SAME offset", off, 0);
    expect_u("  and it is not fresh", fresh, 0);
    expect_u("  reuses counted", a.reuses, 1);

    // a key may not change size under a live VA
    expect_u("the same key at another size is refused", gPlace(&a, kRingVa, 1, 1, 1, kPlane, 512, 1, &off, &fresh),
             N48_RELOC_E_RESIZED);
    expect_u("  and the refusal is counted", a.refused[N48_RELOC_E_RESIZED], 1);

    // find
    expect_u("find locates a placed key", n48_reloc_find(&a, kRect, kRingVa, &off), 1);
    expect_u("  at its offset", off, XLAT12_RELOC_ALIGN);
    expect_u("find misses an unplaced key", n48_reloc_find(&a, kVkba6, kRingVa, &off), 0);

    // bad arguments
    expect_u("key 0 is refused", gPlace(&a, kRingVa, 1, 1, 1, 0, 224, 1, &off, &fresh), N48_RELOC_E_ARG);
    expect_u("zero bytes is refused", gPlace(&a, kRingVa, 1, 1, 1, kVkba6, 0, 1, &off, &fresh), N48_RELOC_E_ARG);

    // the arena bound: 60 KiB, and one program larger than the whole arena
    expect_u("a program larger than the arena is refused",
             gPlace(&a, kRingVa, 1, 1, 1, kVkba6, XLAT12_RELOC_ARENA_BYTES + 1u, 1, &off, &fresh), N48_RELOC_E_FULL);
    expect_u("  the arena is 56 KiB (0.0.413: the last 4 KiB became the fence page)", (uint64_t)XLAT12_RELOC_ARENA_BYTES,
             0xE000u);
    // 0.0.413 (B2): the fence page is the LAST 4 KiB of the reloc arena and MUST NOT overlap the arena; if the two
    // overlapped, a placement could overwrite a fence slot (or vice versa). The planted break is shrinking the arena
    // back to 0xF000 in xlat12_ib.h: this check then fails.
    expect_u("  the fence page does not overlap the arena",
             (uint64_t)(XLAT12_RELOC_ARENA_OFF + XLAT12_RELOC_ARENA_BYTES <= XLAT12_FENCE_PAGE_OFF), 1u);
    expect_u("  the fence page follows the arena exactly",
             (uint64_t)XLAT12_FENCE_PAGE_OFF, (uint64_t)(XLAT12_RELOC_ARENA_OFF + XLAT12_RELOC_ARENA_BYTES));
    expect_u("  the fence page ends where the L1 block begins", (uint64_t)(XLAT12_FENCE_PAGE_OFF + XLAT12_FENCE_PAGE_BYTES),
             0x00A90000u);
    expect_u("  the fence page is one 4 KiB page", (uint64_t)XLAT12_FENCE_PAGE_BYTES, 0x1000u);
    expect_u("  free space is what is left", n48_reloc_free(&a), XLAT12_RELOC_ARENA_BYTES - 2u * XLAT12_RELOC_ALIGN);

    // fill it: rows run out before the bytes do, and both are counted
    std::memset(&a, 0, sizeof a);
    uint32_t placed = 0;
    for (uint32_t i = 0; i < N48_RELOC_ROWS + 4u; i++)
        if (gPlace(&a, kRingVa, 1, 1, 1, 0x1000ull + i, 256, 1, &off, &fresh) == N48_RELOC_OK) placed++;
    expect_u("placements stop at the row table", placed, N48_RELOC_ROWS);
    expect_u("  and the overflow is counted", a.refused[N48_RELOC_E_ROWS], 4);
    expect_u("  offsets are every 256 bytes", a.row[N48_RELOC_ROWS - 1u].off, (N48_RELOC_ROWS - 1u) * XLAT12_RELOC_ALIGN);

    // running out of BYTES, with few rows
    std::memset(&a, 0, sizeof a);
    expect_u("a 32 KiB program fits", gPlace(&a, kRingVa, 1, 1, 1, 0x2001, 32u * 1024u, 1, &off, &fresh), N48_RELOC_OK);
    expect_u("a second 32 KiB program does not", gPlace(&a, kRingVa, 1, 1, 1, 0x2002, 32u * 1024u, 1, &off, &fresh),
             N48_RELOC_E_FULL);
    expect_u("  the arena is full, not the table", a.used, 1);
    expect_u("  and nothing ran past the arena", (uint64_t)a.cursor <= XLAT12_RELOC_ARENA_BYTES, 1);

    // LIFETIME: a new carve or a new shader-cache epoch empties it
    std::memset(&a, 0, sizeof a);
    (void)gPlace(&a, kRingVa, 1, 7, 3, kPlane, 224, 1, &off, &fresh);
    expect_u("placed under carve 7 epoch 3", a.used, 1);
    expect_u("a new CARVE empties the arena", gPlace(&a, kRingVa, 1, 8, 3, kRect, 200, 1, &off, &fresh), N48_RELOC_OK);
    expect_u("  the old placement is gone", a.used, 1);
    expect_u("  and the new one starts at 0", off, 0);
    expect_u("  the old key is no longer found", n48_reloc_find(&a, kPlane, kRingVa, &off), 0);
    (void)gPlace(&a, kRingVa, 1, 8, 3, kPlane, 224, 1, &off, &fresh);
    expect_u("a new EPOCH empties it too", gPlace(&a, kRingVa, 1, 8, 4, kRect, 200, 1, &off, &fresh), N48_RELOC_OK);
    expect_u("  starting at 0 again", off, 0);
    expect_u("  with one row", a.used, 1);

    // every refusal has a name
    for (uint32_t r = 0; r < N48_RELOC_REASONS; r++)
        if (n48_reloc_reason(r)[0] == '?') expect_u("every refusal has a name", r, 0xffff);
    expect_u("reasons: 7", N48_RELOC_REASONS, 7);
}

int main()
{
    checks();
    gPlace = &n48_reloc_place; gFind = &n48_reloc_find;
    checks_base();       // 0.0.399: the real F2 checks count toward the real pass/fail, not only the mutants
    const int realFail = gFail, realRun = gRun;
    std::printf("%d/%d passed\n", realRun - realFail, realRun);

    struct { const char *name; PlaceFn fn; } mut[] = {
        { "M1 a re-placed key gets a NEW offset (a live program moves)", &m1_place },
        { "M2 the cursor is not rounded (the next VA is unaligned)", &m2_place },
        { "M3 the arena bound is not enforced (it runs into the L1 block)", &m3_place },
        { "M4 an unbuilt ring region is accepted", &m4_place },
        { "M5 a key is silently re-placed at another size", &m5_place },
        { "M6 a new carve or epoch does not empty the arena", &m6_place },
    };
    int caught = 0, planted = 0;
    for (auto &m : mut) {
        gQuiet = 1; gFail = 0; gRun = 0;
        gPlace = m.fn;
        checks();
        const int f = gFail, r = gRun;
        gPlace = &n48_reloc_place;
        gQuiet = 0;
        std::printf("mutant %-58s %s (%d of %d checks fail)\n", m.name, f ? "CAUGHT" : "NOT CAUGHT", f, r);
        planted++; if (f) caught++;
    }
    // 0.0.399: the VA base's own checks and their planted defects. The real pass ran above; these are the mutants.
    {
        gPlace = &n48_reloc_place; gFind = &n48_reloc_find;
        struct { const char *name; FindFn fn; } fm[] = {
            { "F2 find ignores the VA base it is asked through (0.0.398)", &m7_find },
        };
        for (auto &m : fm) {
            gQuiet = 1; gFail = 0; gRun = 0;
            gFind = m.fn; gPlace = &n48_reloc_place;
            checks_base();
            const int f = gFail, r = gRun;
            gFind = &n48_reloc_find;
            gQuiet = 0;
            std::printf("mutant %-58s %s (%d of %d checks fail)\n", m.name, f ? "CAUGHT" : "NOT CAUGHT", f, r);
            planted++; if (f) caught++;
        }
        struct { const char *name; PlaceFn fn; } bm[] = {
            { "F2 a moved VA base does not empty the arena", &m8_place },
        };
        for (auto &m : bm) {
            gQuiet = 1; gFail = 0; gRun = 0;
            gFind = &n48_reloc_find; gPlace = m.fn;
            checks_base();
            const int f = gFail, r = gRun;
            gPlace = &n48_reloc_place;
            gQuiet = 0;
            std::printf("mutant %-58s %s (%d of %d checks fail)\n", m.name, f ? "CAUGHT" : "NOT CAUGHT", f, r);
            planted++; if (f) caught++;
        }
    }
    std::printf("mutants caught %d/%d\n", caught, planted);
    return (realFail || caught != planted) ? 1 : 0;
}
