/* gfx_lutfill.h — 0.0.402 ( "DESIGN FOR 0.0.401 (R2)",a,b, and's BINDING FIXES H1-H7):
 * THE IDENTITY LUT, PURE PARTS.
 *
 * WHY. CONFIRMED cause (A) from the shader: GPUPass exports `RGBA = (LUT0.r[s.r], LUT1.r[s.g], LUT2.r[s.b], s.a)`,
 * so with a ZERO slot-4 LUT the presented plane is (0,0,0,s.a) whatever the fill's colour is — exactly arm21 = arm22.
 * The LUT at `0x400240000` was never a write destination of any translated frame this boot (its HEADLESS producers were
 * held back), so R2 (the instrument) writes an IDENTITY ramp into it at verb time and lets GPUPass pass the fill's own
 * colour through. R2 is a one-boot PROOF of (A) and of step 9 on the glass; it is NOT the daily-driver path — R1 (admit
 * the HEADLESS producers through the commit gate) is, and this header says so where a future reader will find it.
 *
 * WHAT LIVES HERE. Everything that can be decided without touching the hardware, so the host suite can drive it and the
 * kext compiles the IDENTICAL code:
 *   - n48_lut_decode_record: the slot-4 record -> (VA, elements, height, slices, bytes), fail-closed;
 *   - n48_lut_instrument_ok: H4 — the ONE record shape THIS instrument writes, and nothing else;
 *   - n48_lut_page_count / n48_lut_page_bytes: the page bookkeeping the learn resolves and the deferred thread walks;
 *   - n48_lut_ramp_dw: the identity value `v_i = i/(N-1)` as the 32-bit float the LUT holds, WITHOUT an FPU;
 *   - n48_lut_batch_page: H1 — "page p, batch off -> `take` dwords" generated INTO the 64-dword batch buffer, so the
 *     thread never holds a whole page on the stack ('s kernel-stack smash);
 *   - n48_lut_plane_shape: H2 — is this frame's translator input list the two-surface plane shape (one tiled, one linear)?
 *   - N48_LUTFILL_FMT: the ONE report line, so its worst-case width is bounded by a test rather than by arithmetic in a
 *     commit message.
 *
 * THE RECORD, AND WHY IT IS ASKED RATHER THAN ASSUMED. wrote "N from the record (80000fff -> width 4096, SUSPECTED)".
 * That reading DROPPED the gfx10 WIDTH_LO field. The slot-4 record measured by is
 * `04002400 c1600000 80000fff c0000204` — an 8-dword gfx10 SQ_IMG_RSRC_WORD0..7 whose WORD1 top two bits are the
 * WIDTH_LO this header decodes. The register databases the translator is generated from (Mesa gfx10-rsrc.json @
 * 4519cc56: SQ_IMG_RSRC_WORD1.WIDTH_LO = bits [31:30], WORD2.WIDTH_HI = bits [11:0], WORD4.DEPTH = bits [12:0];
 * gfx9/gfx10 SQ_RSRC_IMG_1D_ARRAY = 12) give, for that record:
 *   FORMAT 22 = 32_FLOAT, TYPE 12 = 1D_ARRAY, WIDTH = 3 | (4095 << 2) = 16383 -> 16384 elements,
 *   HEIGHT 0 -> 1, DEPTH 2 -> 3 slices, SW_MODE 0 = LINEAR.
 * The same decode on the fixtures confirms it: kT6Mid -> 1920 x 1080 and kT3Win -> 28 x 40 exactly as measured. So the
 * real extent is 3 x 16384 x 4 = 196608 B (192 KiB), NOT the 48 KiBa offered as a fallback. THE FALLBACK IS KEPT
 * HERE ONLY AS A NAMED CONSTANT AND IS NEVER USED FOR A WRITE: fail-closed means an absent or malformed record leaves
 * `ok == 0` and the thread writes nothing. A reviewer should read the 4x disagreement witha as a CORRECTION,
 * not as a silent choice.
 *
 * FAIL-CLOSED BY CONSTRUCTION. `n48_lut_decode_record` returns 0 and fills `why` on EVERY miss; a caller that forgets a
 * field cannot get a green decode. `n48_lut_ramp_dw` is defined for i in [0, n) and returns 0 (0.0) for n <= 1; the
 * thread still refuses a decode whose slices or element count are zero.
 *
 * NO FPU. The kext has no FPU (display_pipe_guard.h:304, "1.0f as an IEEE-754 bit pattern: the kernel has no FPU, so
 * the float is written as an integer"). n48_lut_ramp_dw computes the correctly-rounded single-precision quotient
 * i/(N-1) with integer arithmetic alone, round-to-nearest-even, so it emits no SSE/x87 instruction.
 */
#ifndef N48_GFX_LUTFILL_H
#define N48_GFX_LUTFILL_H

#include <stdint.h>

/* The slot-4 record is Apple's gfx10.3 SQ_IMG_RSRC_WORD0..7 (8 dwords, the image heap's own stride: xlat12_ib.h's
 * XLAT12_IMG_DESC_DWORDS and's "entry index -> heap + index*32" rule put slot 4 at heap + 0x80). */
#define N48_LUT_RECORD_DWORDS 8u
#define N48_LUT_SLOT4_OFF     (4u * 32u)   /* 0x80: image-table entry 4, the slot GPUPass names in slot 4 */
#define N48_LUT_PAGE_BYTES    4096u

/* gfx10 SQ_IMG_RSRC_WORD1.FORMAT bits [28:20]; 22 = GFX10_FORMAT_32_FLOAT (Mesa gfx10-rsrc.json, and the same value in
 * gfx12 after the translator's by-name map). */
#define N48_LUT_FMT_32F 22u
/* gfx10 SQ_IMG_RSRC_WORD3.TYPE bits [31:28]; 12 = SQ_RSRC_IMG_1D_ARRAY (Mesa gfx9/gfx10/gfx81 registers). 13 = 2D_ARRAY
 * and 9 = 2D are accepted too because HEIGHT 1 makes them the same contiguous layout; anything else is refused. */
#define N48_LUT_TYPE_2D       9u
#define N48_LUT_TYPE_1D_ARRAY 12u
#define N48_LUT_TYPE_2D_ARRAY 13u

/* The documenteda fallback (3 x 4096 x 4). NAMED for the record, NEVER used as a write extent: see the block above. */
#define N48_LUT_FALLBACK_ELEMENTS 4096u
#define N48_LUT_FALLBACK_SLICES   3u
#define N48_LUT_FALLBACK_BYTES    ((uint64_t)N48_LUT_FALLBACK_ELEMENTS * N48_LUT_FALLBACK_SLICES * 4u)

/* The decode's refusal reasons, in the order they are tested. 0 = the decode is green. */
enum {
    N48_LUT_OK = 0,
    N48_LUT_ABSENT,      /* a null record pointer, or literally zero dwords (nobody read the heap) */
    N48_LUT_FMT,         /* the record is not 32_FLOAT: the ramp would be the wrong width */
    N48_LUT_TYPE,        /* not a 1D_ARRAY / 2D / 2D_ARRAY: a different layout the ramp's address equation does not cover */
    N48_LUT_SHAPE,       /* slices or elements out of the range the header will write */
    N48_LUT_MISMATCH,    /* the slot-4 record's base disagrees with the held-back HEADLESS producer's CB0 */
    /* 0.0.402. APPENDED, never inserted, so the numbers a previous boot printed keep their meaning. */
    N48_LUT_INSTRUMENT,  /* H4: decodable, but not THIS instrument's record (VA alignment / 16384 elements / 3 slices) */
    N48_LUT_PAGE,        /* H3: a page of the extent did not resolve at learn time */
    N48_LUT_HOST,        /* H3: a page resolved to host memory, not VRAM */
    N48_LUT_STALE,       /* H3: the resolved table's arm scope moved before the deferred write ran */
    /* build 0.0.553 (switch 111, gfx_lutidx111.h). APPENDED. Only switch 111 ON's reads can answer these. */
    N48_LUT_NOT_1D,      /* 111 ON: the draw's own entry is not a 1D_ARRAY record */
    N48_LUT_INVA,        /* 111 ON: the record's VA is not the VA the translator read for the draw's LUT input */
    N48_LUT_REASONS
};

static inline const char *n48_lut_reason_name(uint32_t r)
{
    static const char *const n[N48_LUT_REASONS] = {
        "OK", "record-absent-or-zero", "not-32_FLOAT", "not-a-1D/2D-array", "slice-or-element-count-out-of-range",
        "record-disagrees-with-producer-CB0", "not-this-instrument", "page-not-resolved", "page-is-host-memory",
        "arm-scope-moved", "not-1D_ARRAY (111)", "VA-differs-from-the-draw's-LUT-input (111)" };
    return r < N48_LUT_REASONS ? n[r] : "?";
}

typedef struct {
    uint64_t va;        /* BASE_ADDRESS: (WORD1[7:0] << 40) | (WORD0 << 8), the gfx VA the record names */
    uint32_t elements;  /* WIDTH + 1 (WIDTH is stored as value-1) */
    uint32_t height;    /* HEIGHT + 1 */
    uint32_t slices;    /* DEPTH + 1 */
    uint64_t bytes;     /* elements * height * slices * 4, guarded for overflow */
} n48_lut_desc;

/* The RAW fields of the record, before any shape gate: the test's positive controls read this to prove the WIDTH_LO term
 * against the two fixtures whose measured geometry is known (kT6Mid 1920x1080, kT3Win 28x40), even though neither is a
 * 32_FLOAT LUT record and so neither may pass n48_lut_decode_record. `elements`/`height`/`slices` here are the DECODED
 * (+1) values. A null pointer is a no-op. */
typedef struct { uint64_t va; uint32_t fmt, type, elements, height, slices; } n48_lut_fields;
static inline void n48_lut_fields_of(const uint32_t rec[N48_LUT_RECORD_DWORDS], n48_lut_fields *f)
{
    if (!f) return;
    f->va = 0ull; f->fmt = 0u; f->type = 0u; f->elements = 0u; f->height = 0u; f->slices = 0u;
    if (!rec) return;
    const uint32_t wlo = (rec[1] >> 30) & 0x3u;        /* SQ_IMG_RSRC_WORD1.WIDTH_LO [31:30] */
    const uint32_t whi = rec[2] & 0xfffu;              /* SQ_IMG_RSRC_WORD2.WIDTH_HI [11:0] */
    f->va       = ((uint64_t)(rec[1] & 0xffu) << 40) | ((uint64_t)rec[0] << 8);
    f->fmt      = (rec[1] >> 20) & 0x1ffu;             /* SQ_IMG_RSRC_WORD1.FORMAT [28:20] */
    f->type     = (rec[3] >> 28) & 0xfu;               /* SQ_IMG_RSRC_WORD3.TYPE   [31:28] */
    f->elements = (wlo | (whi << 2)) + 1u;             /* WIDTH is stored as value-1 */
    f->height   = ((rec[2] >> 14) & 0x3fffu) + 1u;     /* SQ_IMG_RSRC_WORD2.HEIGHT [29:14] */
    f->slices   = (rec[4] & 0x1fffu) + 1u;             /* SQ_IMG_RSRC_WORD4.DEPTH  [12:0] */
}

/* The most slices the identity write will ever cover. measured 3; 8 is slack, and anything above it refuses. */
#define N48_LUT_MAX_SLICES 8u

/* 0.0.402 — THIS INSTRUMENT ACCEPTS EXACTLY ONE RECORD SHAPE AND REFUSES EVERY OTHER. measured the LUT as
 * 16384 elements x 3 slices = 196608 B / 48 pages twice: from the descriptor field layout with's 1920x1080 record as
 * the positive control, and from GPUPass's own s10:s11 = 16383/16384 and s12:s13 = 0.5/16384. The ramp is written page by
 * page starting at BASE_ADDRESS, so the base must be page aligned; anything else would place element 0 mid-page. */
#define N48_LUT_INSTRUMENT_ELEMENTS 16384u
#define N48_LUT_INSTRUMENT_SLICES   3u
#define N48_LUT_INSTRUMENT_PAGES    48u
#define N48_LUT_INSTRUMENT_BYTES    ((uint64_t)N48_LUT_INSTRUMENT_ELEMENTS * N48_LUT_INSTRUMENT_SLICES * 4ull)

/* H1: the largest run the thread generates at once. The ramp is generated INTO this buffer inside the batch loop;
 * a whole page (1024 dwords) is NEVER materialised, which is the kernel-stack smash found in 0.0.401. */
#define N48_LUT_BATCH_DWORDS 64u

/* H3: the fixed table the learn resolves every page into, under gXdLock, for the thread to write from. 48 = the
 * instrument's own page count (N48_LUT_INSTRUMENT_PAGES); the learn refuses anything larger rather than truncate it. */
#define N48_LUT_TABLE_PAGES N48_LUT_INSTRUMENT_PAGES

/* Decode the 8-dword slot-4 record. Returns N48_LUT_OK and fills *d, or a refusal reason and leaves *d a defined zero. */
static inline uint32_t n48_lut_decode_record(const uint32_t rec[N48_LUT_RECORD_DWORDS], n48_lut_desc *d)
{
    if (d) { d->va = 0ull; d->elements = 0u; d->height = 0u; d->slices = 0u; d->bytes = 0ull; }
    if (!rec || !d) return N48_LUT_ABSENT;
    if (!rec[0] && !rec[1] && !rec[2] && !rec[3]) return N48_LUT_ABSENT;   /* nothing was read into the record */
    n48_lut_fields f;
    n48_lut_fields_of(rec, &f);
    if (f.fmt != N48_LUT_FMT_32F) return N48_LUT_FMT;
    if (f.type != N48_LUT_TYPE_1D_ARRAY && f.type != N48_LUT_TYPE_2D && f.type != N48_LUT_TYPE_2D_ARRAY) return N48_LUT_TYPE;
    if (f.elements == 0u || f.elements == 0xFFFFFFFFu) return N48_LUT_SHAPE;  /* a wrapped WIDTH is not a ramp */
    if (f.slices == 0u || f.slices > N48_LUT_MAX_SLICES) return N48_LUT_SHAPE;
    /* HEIGHT must be 1: the identity ramp is one contiguous run of `elements` dwords per slice, which is the layout
     * only when the surface is 1 x elements. Anything taller is a different address equation and is refused. */
    if (f.height != 1u) return N48_LUT_SHAPE;
    d->va = f.va;
    d->elements = f.elements;
    d->height   = 1u;
    d->slices   = f.slices;
    const uint64_t per = (uint64_t)d->elements * (uint64_t)d->height * 4ull;
    if (per == 0ull || per > 0xFFFFFFFFull) return N48_LUT_SHAPE;         /* a page/host-bound sized extent, not a 4 GiB one */
    d->bytes = per * (uint64_t)d->slices;
    return N48_LUT_OK;
}

/* 0.0.402 — THE INSTRUMENT'S SHAPE GATE. A record that decodes green is not yet THIS instrument's record: the
 * ramp is proved on ONE shape (a page-aligned base, 16384 elements, 3 slices), and every other decodable record is refused
 * here rather than written with an extent the identity proof does not cover. Pure, so the host suite pins it. Returns
 * N48_LUT_OK or N48_LUT_INSTRUMENT, and (like the decode) is defined for a null descriptor. */
static inline uint32_t n48_lut_instrument_ok(const n48_lut_desc *d)
{
    if (!d) return N48_LUT_ABSENT;
    if ((d->va & 0xfffull) != 0ull) return N48_LUT_INSTRUMENT;
    if (d->elements != N48_LUT_INSTRUMENT_ELEMENTS) return N48_LUT_INSTRUMENT;
    if (d->height != 1u) return N48_LUT_INSTRUMENT;
    if (d->slices != N48_LUT_INSTRUMENT_SLICES) return N48_LUT_INSTRUMENT;
    if (d->bytes != N48_LUT_INSTRUMENT_BYTES) return N48_LUT_INSTRUMENT;
    return N48_LUT_OK;
}

/* Pages [0, bytes) crosses. A zero extent is 0 pages and the caller refuses on that. */
static inline uint32_t n48_lut_page_count(uint64_t bytes)
{
    if (bytes == 0ull || bytes > (0xFFFFFFFFull)) return 0u;
    return (uint32_t)((bytes + (uint64_t)N48_LUT_PAGE_BYTES - 1ull) / (uint64_t)N48_LUT_PAGE_BYTES);
}

/* Bytes in page `p` of `bytes` total: N48_LUT_PAGE_BYTES except the last (the extent need not be page-aligned). */
static inline uint32_t n48_lut_page_bytes(uint32_t p, uint64_t bytes)
{
    const uint32_t np = n48_lut_page_count(bytes);
    if (p >= np) return 0u;
    const uint64_t start = (uint64_t)p * (uint64_t)N48_LUT_PAGE_BYTES;
    const uint64_t left = bytes - start;
    return left > (uint64_t)N48_LUT_PAGE_BYTES ? N48_LUT_PAGE_BYTES : (uint32_t)left;
}

/* The identity value for element `i` of an `n`-element slice, as the 32-bit float the LUT holds: v_i = i/(n-1),
 * round-to-nearest-even, computed with integer arithmetic only (the kernel has no FPU). i is clamped into [0, n-1];
 * n <= 1 answers 0.0. i == 0 answers +0.0 and i == n-1 answers exactly 1.0f, both as the ramp's own endpoints. */
static inline uint32_t n48_lut_ramp_dw(uint32_t i, uint32_t n)
{
    if (n <= 1u || i == 0u) return 0x00000000u;   /* +0.0f */
    const uint32_t d = n - 1u;
    if (i >= d) return 0x3F800000u;               /* 1.0f */
    uint64_t num = (uint64_t)i, den = (uint64_t)d;
    int k = 0;                                    /* v = i/d lies in [2^-k, 2^-(k-1)), k >= 1 */
    while (num < den) { num <<= 1; k++; }
    /* s = v * 2^k = num/den in [1,2). Its 24-bit significand is floor(s * 2^23) rounded to nearest even. */
    const uint64_t scaled = num << 23;            /* < 2^56 for i < 2^32 */
    uint64_t q = scaled / den;
    const uint64_t r = scaled % den;
    if ((r << 1) > den || ((r << 1) == den && (q & 1u))) q++;
    if (q == (1ull << 24)) { q >>= 1; k--; }      /* s rounded up to 2.0: exponent steps up, mantissa returns to 0 */
    const uint32_t biased = (uint32_t)(127 - k);  /* normal only: v >= 2^-32 > 2^-126 on every input this header accepts */
    return (biased << 23) | (uint32_t)(q & 0x7FFFFFu);
}

/* Split a LINEAR dword index `g` of the whole surface into (slice, i) on the layout the record names: slice-major,
 * `elements` dwords per slice, each slice the identity ramp. Returns 0 when g is out of range (the caller refuses). */
static inline uint32_t n48_lut_split(const n48_lut_desc *d, uint32_t g, uint32_t *slice, uint32_t *i)
{
    if (slice) *slice = 0u;
    if (i) *i = 0u;
    if (!d || !slice || !i || d->elements == 0u) return 0u;
    const uint64_t total = (uint64_t)d->elements * (uint64_t)d->slices;
    if ((uint64_t)g >= total) return 0u;
    *slice = (uint32_t)((uint64_t)g / (uint64_t)d->elements);
    *i     = (uint32_t)((uint64_t)g % (uint64_t)d->elements);
    return 1u;
}

/* 0.0.402 — "PAGE p, BATCH off -> `take` dwords", INTO THE CALLER'S 64-DWORD BUFFER. The thread calls this INSIDE
 * its batch loop, so the ramp is generated `take` dwords at a time and a whole page is never materialised on the stack. The
 * batch is a run of the surface's LINEAR dword indices starting at page p's dword `off`; each dword is the identity value of
 * its own (slice, element) position, so every slice carries the same ramp. Returns 0 — and writes nothing — when page/off/
 * take run past the surface or `take` exceeds N48_LUT_BATCH_DWORDS; the caller refuses on the 0. Pure, host-tested against
 * the whole-ramp reference for every page. */
static inline uint32_t n48_lut_batch_page(const n48_lut_desc *d, uint32_t page, uint32_t off, uint32_t take,
                                          uint32_t out[N48_LUT_BATCH_DWORDS])
{
    if (!d || !out || take == 0u || take > N48_LUT_BATCH_DWORDS) return 0u;
    const uint32_t pdw = N48_LUT_PAGE_BYTES / 4u;
    if ((uint64_t)off + (uint64_t)take > (uint64_t)pdw) return 0u;
    const uint64_t g0 = (uint64_t)page * (uint64_t)pdw + (uint64_t)off;
    const uint64_t total = (uint64_t)d->elements * (uint64_t)d->slices;
    if (g0 + (uint64_t)take > total) return 0u;      /* fail-closed before any narrowing cast */
    for (uint32_t j = 0u; j < take; j++) {
        uint32_t slice = 0u, i = 0u;
        if (!n48_lut_split(d, (uint32_t)(g0 + (uint64_t)j), &slice, &i)) return 0u;
        (void)slice;                                  /* every slice carries the SAME identity ramp */
        out[j] = n48_lut_ramp_dw(i, d->elements);
    }
    return 1u;
}

/* 0.0.402 — IS THIS FRAME THE TWO-SURFACE PLANE SHAPE? found the committed plane frame binds exactly TWO
 * images: the composited surface in image slot 1 (TILED) and the LUT in slot 4 (LINEAR). `mode` is the translator's own
 * `xlat12_draw_stats.in_mode[]` — its TRANSLATED gfx12 SW_MODE, 0 = linear — so the answer is read from the translator's
 * own stats, never re-derived here. Exactly two inputs with one linear and one tiled is the shape; the input list's ORDER
 * is not assumed. Anything else (fewer, more, both tiled, both linear) returns 0 and the learn does not run. Pure. */
static inline uint32_t n48_lut_plane_shape(const uint32_t *mode, uint32_t n)
{
    if (!mode || n != 2u) return 0u;
    const uint32_t l0 = (mode[0] == 0u) ? 1u : 0u;
    const uint32_t l1 = (mode[1] == 0u) ? 1u : 0u;
    return (l0 != l1) ? 1u : 0u;      /* exactly one linear and one tiled */
}

/* THE ONE REPORT LINE, in one place so its worst case is bounded by a test rather than by arithmetic in a commit
 * message. Args: switch state, change word, LUT VA, slices, elements, bytes, pages, walked,
 * written, read back, mismatched, ready, refused-until-ready, dest-check word, hdp word, tail (a reason or empty).
 * 0.0.402: the switch state and the change word are separated by " - " — through 0.0.401 the format was
 * `is %s%s.`, so the line read `is ON`gfxneuter 32` CHANGED it.` with no separator. */
#define N48_LUTFILL_FMT \
    "lutfill: `gfxneuter 32 | M << 8` is %s - %s. LUT VA %#llx: %u slice(s) x %u x 4 B = %llu B (%u page(s)); " \
    "walked %u, written %u, read back %u, mismatched %u; ready %u, refused-until-ready %llu; dest-check %s, " \
    "HDP flush %s. %s"

/* The worst case the test measures against: HWLOG's 512-byte line minus "AppleHardwareHook: " (19) and "\n" (1); the
 * brief asks for a BODY under 480 B, which is stricter. gfx_commit.h's N48_LOG_CAP_BODY is 491. */
#ifndef N48_LOG_CAP_BODY
#define N48_LOG_CAP_BODY 491u
#endif
#define N48_LUTFILL_BODY_CAP 480u   /* the brief's own ceiling for this line */

#endif /* N48_GFX_LUTFILL_H */
