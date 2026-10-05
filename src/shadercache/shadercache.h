/*
 * shadercache.h - hash-keyed gfx1201 shader cache: blob parsing and lookup (milestone 3).
 *
 * Kext-ready: plain C that also compiles as C++; no heap, no floating point, no exceptions,
 * no libc beyond <stdint.h>/<stddef.h>; every read of the blob is bounds-checked and
 * little-endian explicit. The spec (key site, key, blob format, evidence) is README.md.
 *
 * Model of use at the residency copy (README "Integration"):
 *   sc_open() once on the cache blob; then for a resource's bytes sc_scan() (or sc_lookup() at
 *   each 0x100-grid offset). A match with SC_F_SUBSTITUTE gives, through sc_subst_render(), the
 *   exact bytes to write at the code's VRAM address; sc_adjust_get() gives the register
 *   adjustments an IB hook must apply when the entry carries SC_F_HAS_ADJUST.
 */
#ifndef N48_SHADERCACHE_H
#define N48_SHADERCACHE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- format constants (README "Blob format") ------------------------------------------ */
#define SC_VERSION_MAJOR      1u
#define SC_VERSION_MINOR      0u
#define SC_HEADER_SIZE        0x60u
#define SC_INDEX_REC_SIZE     16u
#define SC_MASKSET_REC_SIZE   8u
#define SC_ENTRY_FIXED_SIZE   0x50u
#define SC_ADJ_REC_SIZE       20u
#define SC_HASH_FNV1A64       1u
#define SC_TERMINATOR         0xbf810000u        /* gfx10 `s_endpgm` (operand 0) */
#define SC_FNV64_BASIS        0xcbf29ce484222325ull
#define SC_FNV64_PRIME        0x100000001b3ull
#define SC_MAX_KEY_DWORDS     0x4000u            /* 64 KiB of code before the terminator */
#define SC_MAX_APPLE_DWORDS   0x10000u
#define SC_MAX_ENTRIES        0x100000u
#define SC_MAX_MASKSETS       64u
#define SC_MAX_MASK_INDICES   1024u
#define SC_MAX_ADJUST         256u
#define SC_MAX_NAME           255u

/* status codes: 0 = hit, 1 = clean miss, negative = refusal */
enum {
    SC_OK = 0,
    SC_MISS = 1,
    SC_E_ARG = -1,
    SC_E_SHORT = -2,       /* blob shorter than its header says */
    SC_E_MAGIC = -3,
    SC_E_VERSION = -4,
    SC_E_HEADER = -5,      /* a header field out of its allowed range */
    SC_E_BOUNDS = -6,      /* a region or record outside the blob */
    SC_E_ORDER = -7,       /* index not sorted */
    SC_E_ENTRY = -8,       /* an entry record is inconsistent */
    SC_E_KEY = -9,         /* stored key != key recomputed from stored Apple bytes */
    SC_E_CHECKSUM = -10,
    SC_E_MASK = -11,
    SC_E_STRING = -12,
    SC_E_AMBIGUOUS = -13,  /* more than one verified entry matched: never substitute */
    SC_E_NOSUB = -14,      /* the match is not substitutable (known-only, conflict, hash-only) */
    SC_E_CAPACITY = -15,   /* output buffer or Apple allocation too small */
    SC_E_ADJUST = -16      /* entry needs register adjustments the caller did not accept */
};

/* entry flags */
#define SC_F_SUBSTITUTE   0x1u   /* carries gfx1201 code that may replace Apple's */
#define SC_F_CONFLICT     0x2u   /* the harvest mapped these Apple bytes to different values */
#define SC_F_HASH_ONLY    0x4u   /* only the key is known (no Apple bytes to compare) */
#define SC_F_HAS_ADJUST   0x8u   /* substitution also needs register adjustments (IB hook) */
/* 0.0.311 (notes §565): WE HOLD CODE FOR THIS KEY, BUT IT DOES NOT FIT APPLE'S ALLOCATION.
 * SC_F_SUBSTITUTE means "may replace Apple's program in place", which is a claim about SIZE as much as about content.
 * When the image is larger than the record's capacity it cannot be written there; with this flag the image still ships,
 * the relocation arena places it in memory WE own, and the translated draw points SPI_SHADER_PGM_LO/HI_ES at that copy
 * instead (xlat12_ib.c case 0xb120). The two flags are alternatives, never both: one writes over Apple's program, the
 * other deliberately leaves it untouched.
 *
 * 0.0.318 (notes §569, §585) - THE EXAMPLE THIS COMMENT USED TO GIVE WAS WRONG AND IS REMOVED. It read: "Apple's
 * RectPosTexFast_VS is 128 bytes of code: our gfx1201 build of it measures 200 and cannot go there." The 128 was a
 * DYLIB SYMBOL EXTENT, not the allocation. §569 withdrew it (PGM_LO holds VA >> 8, so nothing can sit at +0x80, and
 * the cache record's own capacity has read 0x100 throughout), and the kext printed the real number on hardware in run
 * m4c9: "a value may never exceed Apple's own 256-byte allocation". 200 B into 256 B FITS, and that program is
 * substituted in place from 0.0.318.
 *
 * WHICH FLAG TO SHIP, AND WHY IT IS NOT A FREE CHOICE: if the image fits the record's capacity, SUBSTITUTE. Relocation
 * is the safer mechanism only for something that does not fit; for something that does it is strictly worse, because
 * aiming PGM_LO/HI at our arena puts the mapVA root write and SQ instruction fetch from OUR leaves on the critical
 * path, and neither has ever been shown to work (the root write refuses 3/3 at G3 structurally, and the ring fetches
 * once read as proving our mapping fetch-capable are DATA reads gated by READABLE, not EXECUTABLE). Measure the fit
 * against the record's capacity - never against a symbol extent, which is the mistake that cost two days. */
#define SC_F_RELOCATE     0x10u
#define SC_F_ALL          0x1fu

/* stages */
enum { SC_STAGE_UNKNOWN = 0, SC_STAGE_COMPUTE = 1, SC_STAGE_VERTEX = 2, SC_STAGE_FRAGMENT = 3 };

/* why sc_lookup missed (sc_match.miss_reason) */
enum {
    SC_MISS_NONE = 0,
    SC_MISS_EMPTY = 1,     /* first dword is zero: no code starts here */
    SC_MISS_NOEND = 2,     /* no terminator within max_key_dwords / the bytes available */
    SC_MISS_NOKEY = 3,     /* no index record has this key */
    SC_MISS_COMPARE = 4,   /* key present, bytes differ (hash collision or prefix-sharing code) */
    SC_MISS_SHORT = 5      /* key present, fewer bytes available than the entry compares */
};

typedef struct sc_cache {
    const uint8_t *base;
    uint32_t size;
    uint32_t grid;
    uint32_t max_key_dwords, max_apple_dwords;
    uint32_t entry_count;
    uint32_t index_off;
    uint32_t entries_off, entries_size;
    uint32_t masks_off, masks_count;
    uint32_t words_off, words_size;
    uint32_t strings_off, strings_size;
} sc_cache;

typedef struct sc_match {
    uint64_t key;
    uint32_t entry_off;        /* absolute offset of the entry in the blob */
    uint16_t flags, stage;
    uint16_t mask_set, key_dwords;
    uint32_t apple_dwords;     /* dwords compared (0 for hash-only) */
    uint32_t capacity_bytes;   /* bytes of Apple's allocation from the code start */
    uint32_t subst_dwords;
    uint32_t adj_count;
    uint32_t verified;         /* 1: every unmasked Apple dword compared equal */
    uint32_t miss_reason;
} sc_match;

typedef struct sc_adjust {
    uint32_t reg;              /* gfx10.3 register byte address (as the IB/record names it) */
    uint32_t guard_mask, guard_value;   /* apply only if (apple_value & guard_mask) == guard_value */
    uint32_t set_mask, set_value;       /* new = (apple_value & ~set_mask) | set_value */
} sc_adjust;

typedef struct sc_scan_stats {
    uint32_t candidates, matches, ambiguous, errors;
    uint32_t miss_empty, miss_noend, miss_nokey, miss_compare, miss_short;
} sc_scan_stats;

/* callback: return 0 to continue scanning, nonzero to stop */
typedef int (*sc_scan_fn)(void *ctx, size_t offset, const sc_match *m);

/* ---- hashing and the key (README "The key") ------------------------------------------- */
uint64_t sc_fnv1a64(uint64_t h, const uint8_t *p, size_t n);
/* FNV-1a-64 over u32le(key_dwords) || code[0 .. 4*key_dwords) with each masked dword read as
 * zero. mask_idx must be strictly ascending; indices >= key_dwords do not enter the key. */
uint64_t sc_key(const uint8_t *code, uint32_t key_dwords, const uint32_t *mask_idx, uint32_t mask_n);
/* dwords through the first SC_TERMINATOR, scanning at most max_dwords and avail bytes */
int sc_extent(const uint8_t *buf, size_t avail, uint32_t max_dwords, uint32_t *key_dwords);

/* ---- the cache ------------------------------------------------------------------------- */
int sc_open(sc_cache *c, const uint8_t *blob, size_t len);
uint32_t sc_entry_count(const sc_cache *c);
int sc_entry_at(const sc_cache *c, uint32_t index_pos, sc_match *m);
int sc_lookup(const sc_cache *c, const uint8_t *buf, size_t avail, sc_match *m);
/* All attempted starts, including empty slots, have exactly one terminal bucket.
 * matches includes known-only / callback refusals; those are NOT additional misses. */
uint64_t sc_scan_total(const sc_scan_stats *st);
/* Windows own starts in [base, base + owned); len includes lookahead. Initialise
 * *next = 0 once per resource, retain it across windows (including callback refusals).
 * base and non-final owned lengths must be grid aligned, with no gaps. Non-final
 * windows need max(key, compare) bytes of lookahead. Callback offsets are window-local.
 * A stopped callback advances next past its match so resumption never repeats it. */
int sc_scan_window(const sc_cache *c, const uint8_t *buf, size_t len,
                   size_t base, size_t owned, uint32_t grid, size_t *next,
                   sc_scan_fn fn, void *ctx, sc_scan_stats *st);
int sc_scan(const sc_cache *c, const uint8_t *buf, size_t len, uint32_t grid,
            sc_scan_fn fn, void *ctx, sc_scan_stats *st);
int sc_subst_render(const sc_cache *c, const sc_match *m, int accept_adjust,
                    uint8_t *out, size_t out_cap, uint32_t *nbytes);
/* The same bytes sc_subst_render would write, for an entry reached by sc_entry_at rather than by a
 * verified lookup. It is for RECOGNISING code that was already substituted: after substitution the
 * bytes at the shader's address are gfx1201 and end in 0xbfb00000, so they no longer carry the gfx10
 * terminator the key is built on and sc_lookup can never find them again (README "The key"). A caller
 * compares these bytes with what it reads at the address; equality identifies the entry.
 * It does NOT authorise a substitution: it ignores `verified` and adjustments, so it must never be
 * used to decide what to write. Refuses a non-substitutable entry (SC_E_NOSUB). */
int sc_subst_image(const sc_cache *c, const sc_match *m, uint8_t *out, size_t out_cap, uint32_t *nbytes);
int sc_adjust_get(const sc_cache *c, const sc_match *m, uint32_t i, sc_adjust *a);
/* BUILD TASK 0.0.536 (notes §1322; switch 92 in the kext, gfx_rv92.h): THE ALTERNATIVE IMAGE of a verified match. An
 * alternative is a second SUBSTITUTE entry for the same Apple program that a lookup can never verify on its own: same key,
 * mask set and key length, same flags, stage and capacity, MORE Apple dwords whose first m->apple_dwords equal the verified
 * entry's stored ones (tools/gfx-cache-build-r20.py: Apple's bytes followed by non-zero marker dwords the slot's zero padding
 * never matches). SC_OK and *alt (verified = 1: its Apple prefix is exactly the bytes `m` verified) when exactly one exists;
 * SC_MISS when none; SC_E_AMBIGUOUS when more than one (nothing chosen); SC_E_NOSUB when `m` is not a verified SUBSTITUTE
 * match; *alt is zeroed on every path but SC_OK. Pure over the blob; the lookup itself is unchanged. */
int sc_alt_find(const sc_cache *c, const sc_match *m, sc_match *alt);
const char *sc_entry_name(const sc_cache *c, const sc_match *m, uint32_t *len);
const char *sc_status_name(int status);

#ifdef __cplusplus
}
#endif
#endif /* N48_SHADERCACHE_H */
