// gfx_subst_caps.h — build 0.0.484 (an internal design note Q2 K1-K7, Q6). THE PROGRAM AND IMAGE LIMITS OF THE
// SOURCE HOOK'S PROGRAM IDENTIFICATION, IN ONE PLACE, AND THE PURE HALF OF WHAT THEY SIZE.
//
// WHY THIS FILE EXISTS. glass_background_lph (BD 0x597b70b28759e040, BE 0x5abbb918af0075b7) blocks the login screen's
// frames at program-unknown ("key 0"). Its gfx1201 images are 5084 B (BD, 1271 dwords) and 5076 B (BE, 1269), which fit
// Apple's own 5376-byte allocation; but a substitution writes max(ours, Apple's) bytes, so ANY glass image renders to
// 5376 B, and four separate limits stood between that image and a frame (GLASS.md Q2's table):
//   K1 kScMaxSubstBytes (Navi48AccelPeer.cpp, gScOut): the residency writer's output buffer. sc_subst_render refuses
//      SC_E_CAPACITY when it is smaller than the write, so at 2048 glass was never substituted at all.
//   K2 kXdSubstBytes (AppleHardwareHook.cpp): one row of the recognition pool gfxsrc_xlat_open renders every
//      substitutable image into. At 2048 the glass image lands in `tooBig` and can never be recognised as OURS.
//   K3 kXdPgmDwords (AppleHardwareHook.cpp): gfxsrc_read_program's second read. Our gfx1201 bytes carry no gfx10
//      terminator, so recognition reads the FULL window and needs got * 4 >= the image's bytes: 1344 dwords.
//   K4 N48_PGMID_CAP (gfx_pgmid.h): the head-first identity read (switch 38 modes T/T+M), >= 1271.
// Each lived as a literal beside its own buffer, with nothing tying them together; GLASS.md K6 found that raising K4
// alone would read past gXdPgm. The constants now live HERE, AppleHardwareHook.cpp and Navi48AccelPeer.cpp take their
// own names from them, and the static_asserts below make the dangerous combinations fail to compile - in the kext AND
// in every host test that includes this header.
//
// THE PURE HALF (the same code the kext runs, so the host suites test it rather than a copy):
//   n48_xd_read_program - gfxsrc_read_program's two-step read (the rest of the page, then the full K3 window when no
//                         gfx10 terminator was in it), over a caller-supplied reader.
//   n48_xd_ours_find    - "are these bytes one of OUR rendered images?", the byte compare both the identification
//                         (gfxsrc_identify_pgm) and the no-identity witness (gfxsrc_note_no_identity) run.
// The pool BUILD (which needs the shader-cache library) is gfx_subst_pool.h.
#ifndef N48_GFX_SUBST_CAPS_H
#define N48_GFX_SUBST_CAPS_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "gfx_pgmid.h"

// K3 — gfxsrc_read_program's full window, and the width of gXdPgm (IOMalloc'd once, kXdPgmDwords * 4 = 5376 B).
// 1344 dwords = 5376 B = Apple's allocation for glass (GLASS.md Q1: "Trimmed for the blob ... at capacity 5376"), the
// largest image any shipped entry renders to. Was 1024 (: "the longest Apple program in any record we hold is L = 335").
#define N48_XD_PGM_DWORDS      1344u
// K2 — one row of the recognition pool (gXdSubst, IOMalloc'd once, N48_XD_SUBST_MAX rows). Was 2048 (: "Apple's
// app-shader allocations run 512-1792 B"); glass renders to 5376.
#define N48_XD_SUBST_BYTES     5376u
// THE SUBSTITUTION CAP — how many substitutable entries gfxsrc_xlat_open renders for recognition. Was 96; r17 has 90
// substitutable entries and r18 (r17 + BD + BE + compute N) 92-93. 128 leaves 35 of headroom; an entry past the cap is
// never rendered, so a program it substituted could never be recognised as ours - gfx_subst_pool.h now COUNTS those
// (n48_xd_subst_counts.dropped) and the kext logs the count at boot and at the pool's build. The pool is heap, not
// stack: N48_XD_SUBST_MAX * N48_XD_SUBST_BYTES = 128 * 5376 = 688,128 B (672 KiB), from 96 * 2048 = 196,608 B.
#define N48_XD_SUBST_MAX       128u
// K5 — kXdIdDwords, the identity window mode OFF/SHADOW reads (and the no-identity witness is grown to). LEFT AT 512 on
// purpose (GLASS.md Q2 K5: raising it costs EVERY OFF-mode ask about 2.6x in MM traffic,): glass is then
// identifiable only under switch 38 mode T/T+M, which is what the decide recipes run (`gfxneuter-550`), and the boot
// guard (AppleHardwareHook.cpp gfxsrc_idcap_boot_report) NAMES every identity row this window cannot reach.
#define N48_XD_ID_DWORDS       512u
// K1 — kScMaxSubstBytes, the residency writer's gScOut (a file-scope static in Navi48AccelPeer.cpp: .bss 2048 -> 5376).
#define N48_SC_MAX_SUBST_BYTES 5376u

// K6 — THE INVARIANTS (GLASS.md Q2 K6: "No static_assert ties them: raising K4 without K3 is a heap overflow").
// gfxsrc_pgm_profile's OFF/SHADOW read lands kXdIdDwords dwords in gXdPgm; mode T lands up to N48_PGMID_CAP there.
static_assert(N48_XD_ID_DWORDS <= N48_XD_PGM_DWORDS,
              "kXdIdDwords (K5) reads land in gXdPgm, which is kXdPgmDwords (K3) wide - raise K3 first");
static_assert(N48_PGMID_CAP <= N48_XD_PGM_DWORDS,
              "a head-first read of N48_PGMID_CAP (K4) dwords lands in gXdPgm, which is kXdPgmDwords (K3) wide - "
              "raising K4 without K3 reads past the buffer; raise K3 first");
// the witness extension grows a head-first read to kXdIdDwords inside a buffer at least N48_PGMID_CAP wide (gXdPgmT)
static_assert(N48_XD_ID_DWORDS <= N48_PGMID_CAP,
              "the head-first read's witness extension grows to kXdIdDwords; its buffers are N48_PGMID_CAP wide");
// a rendered image is recognised only when the read that found it holds all of its bytes (got * 4 >= nb): an image row
// wider than K3's window could never be recognised
static_assert(N48_XD_SUBST_BYTES <= 4u * N48_XD_PGM_DWORDS,
              "a recognition image wider than gfxsrc_read_program's window (K3) can never be matched");
static_assert(N48_XD_SUBST_BYTES % 4u == 0u && N48_SC_MAX_SUBST_BYTES % 4u == 0u, "images are whole dwords");

// One rendered image's row in the recognition pool: the entry's key, the bytes sc_subst_image rendered (nb), its stage.
// Image i lives at pool + i * N48_XD_SUBST_BYTES.
typedef struct { uint64_t key; uint32_t bytes; uint32_t stage; } n48_xd_subst_row;

// The byte compare. 1 and *key = the row's key when the `got` dwords at `pgm` BEGIN WITH one rendered image (every
// byte of it: a read shorter than the image never matches, whatever its prefix); 0 otherwise. Rows are tried in pool
// order and the first match wins, exactly the loop it replaces at both kext sites.
static inline int n48_xd_ours_find(const uint32_t *pgm, uint32_t got, const uint8_t *pool, const n48_xd_subst_row *rows,
                                   uint32_t n, uint64_t *key)
{
    if (!pgm || !pool || !rows) return 0;
    for (uint32_t i = 0; i < n; i++) {
        const uint32_t nb = rows[i].bytes;
        if (nb == 0 || (size_t)got * 4u < nb) continue;
        if (memcmp(pgm, pool + (size_t)i * N48_XD_SUBST_BYTES, nb) == 0) {
            if (key) *key = rows[i].key;
            return 1;
        }
    }
    return 0;
}

// The reader the kext hands n48_xd_read_program: gfxc_read over the submitter's page table (AppleHardwareHook.cpp's
// gfxsrc_read_cb); a host test hands it a mock VM. Returns the dwords actually read into dst (0 = nothing).
typedef uint32_t (*n48_xd_read_fn)(void *ctx, uint64_t va, uint32_t *dst, uint32_t ndw);

// gfxsrc_read_program, moved here unchanged apart from its window's name: read the rest of the program's own page
// first, and the full N48_XD_PGM_DWORDS window only if no gfx10 terminator (0xBF810000) was in it. *L = the dwords
// through the first terminator, 0 when none was read (always 0 for our gfx1201 images, which end in 0xBFB00000).
// `buf` must hold N48_XD_PGM_DWORDS dwords.
static inline uint32_t n48_xd_read_program(n48_xd_read_fn rd, void *ctx, uint64_t va, uint32_t *buf, uint32_t *L)
{
    const uint32_t inPage = (uint32_t)((0x1000ull - (va & 0xfffull)) / 4ull);
    const uint32_t want = inPage < N48_XD_PGM_DWORDS ? inPage : N48_XD_PGM_DWORDS;
    uint32_t got = rd(ctx, va, buf, want);
    *L = 0;
    for (uint32_t i = 0; i < got; i++) if (buf[i] == 0xBF810000u) { *L = i + 1u; break; }
    if (!*L && got == want && want < N48_XD_PGM_DWORDS) {
        got = rd(ctx, va, buf, N48_XD_PGM_DWORDS);
        for (uint32_t i = 0; i < got; i++) if (buf[i] == 0xBF810000u) { *L = i + 1u; break; }
    }
    return got;
}

#endif /* N48_GFX_SUBST_CAPS_H */
