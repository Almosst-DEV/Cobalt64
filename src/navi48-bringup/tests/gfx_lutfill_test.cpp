// gfx_lutfill_test.cpp — 0.0.401 ( "DESIGN FOR 0.0.401 (R2)",a items 1-5,b): THE IDENTITY LUT'S PURE
// PARTS, offline. The property under test is the one the hardware run is scored on and the one a wrong decode destroys:
//
//     the slot-4 record 04002400 c1600000 80000fff c0000204 names the LUT's REAL extent, and the identity ramp written
//     into it is v_i = i/(N-1) as a 32-bit float.
//
// WHAT IS CHECKED, and why each is a falsifier rather than a formality:
//   1. THE REAL RECORD decodes to VA 0x400240000, 16384 elements x 3 slices, 192 KiB, 48 pages. The width comes from
//      BOTH the gfx10 WIDTH_LO field (WORD1[31:30]) AND WIDTH_HI (WORD2[11:0]);'s "4096 / 48 KiB" dropped
//      WIDTH_LO. The two measured fixtures (1920x1080 and 28x40) are the positive controls for that term.
//   2. THE IDENTITY RAMP. First element is +0.0f, last is exactly 1.0f, and EVERY element equals the host's own
//      correctly-rounded `i/(N-1)` — so the FPU-free integer routine is proved against the definition, not against itself.
//   3. PAGE BOOKKEEPING. page_count/page_bytes for the real extent, a non-page-aligned extent, and the (slice, element)
//      split at every boundary.
//   4. THE ONE REPORT LINE fits the brief's 480-byte body cap at its widest arguments.
//   5. NON-VACUITY: four defects are planted in COPIES of the pure code and each must be caught by the checks above.
//      (The real header was also broken and restored by hand during the build; see the notes.)
//
//     clang++ -std=c++17 -Wall -Wextra -Werror -O1 -fsanitize=address,undefined -fno-sanitize-recover=all \
//         -I src/navi48-bringup/src/apple src/navi48-bringup/tests/gfx_lutfill_test.cpp -o /tmp/luttest && /tmp/luttest
#include <cstdio>
#include <cstdint>
#include <cstring>
#include "gfx_lutfill.h"
#include "gfx_lutidx111.h"   // build 0.0.553: switch 111, section 111 below

static int gFail = 0, gRun = 0, gQuiet = 0;

static void expect_u(const char *what, uint64_t got, uint64_t want)
{
    gRun++;
    if (got != want) {
        gFail++;
        if (!gQuiet) std::printf("FAIL  %-74s got %#llx want %#llx\n", what, (unsigned long long)got, (unsigned long long)want);
    } else if (!gQuiet) {
        std::printf("ok    %-74s %#llx\n", what, (unsigned long long)got);
    }
}

// The host's own float for a quotient, as bits: the definition the integer ramp must reproduce exactly.
static uint32_t host_float_bits(uint32_t i, uint32_t n)
{
    if (n <= 1u || i == 0u) return 0x00000000u;
    if (i >= n - 1u) return 0x3F800000u;
    const float v = (float)i / (float)(n - 1u);
    uint32_t u = 0;
    std::memcpy(&u, &v, sizeof u);
    return u;
}

// The real slot-4 record. WORD0/1 base = 0x400240000, WORD1 FORMAT 22 = 32_FLOAT, WIDTH_LO 3,
// WORD2 WIDTH_HI 0x0fff -> WIDTH 16383 (+1 = 16384), HEIGHT 0 (+1 = 1); WORD3 TYPE 12 = 1D_ARRAY;
// WORD4 DEPTH 2 (+1 = 3 slices).
static const uint32_t kLutRecord[N48_LUT_RECORD_DWORDS] = {
    0x04002400u, 0xc1600000u, 0x80000fffu, 0xc0000204u, 0x00000002u, 0x00400000u, 0x00000000u, 0x00000000u
};
// The measured fixtures used as POSITIVE CONTROLS for the WIDTH_LO term (tests/fixture_arm13_*;). They are
// 8_8_8_8 records, so the FORMAT field is patched to 32_FLOAT (22) before a decode that is about the DIMENSIONS.
static const uint32_t kMidRecord[N48_LUT_RECORD_DWORDS] = {   // the composited surface: 1920 x 1080, SW 27
    0x04008000u, 0xc3800000u, 0x810dc1dfu, 0x99b00f2eu, 0x00000000u, 0x00400000u, 0x00000200u, 0x00000000u };
static const uint32_t kWinRecord[N48_LUT_RECORD_DWORDS] = {   // f2's one 28 x 40 image, SW 22
    0x04000060u, 0xc3800000u, 0x8009c006u, 0x99600f2eu, 0x00000000u, 0x00400000u, 0x00000200u, 0x00000000u };

// ---------------------------------------------------------------------------------------------------------------------
// 1. the decode
// ---------------------------------------------------------------------------------------------------------------------
static void decode_checks()
{
    n48_lut_desc d;
    expect_u("decode: the real record is green", n48_lut_decode_record(kLutRecord, &d), N48_LUT_OK);
    expect_u("decode: VA is 0x400240000", d.va, 0x400240000ull);
    expect_u("decode: WIDTH_LO is honoured (16384 elements, not 4096)", d.elements, 16384u);
    expect_u("decode: HEIGHT + 1", d.height, 1u);
    expect_u("decode: DEPTH + 1 = 3 slices", d.slices, 3u);
    expect_u("decode: the extent is 192 KiB (3 x 16384 x 4)", d.bytes, 196608ull);
    expect_u("decode: the extent is NOT the 48 KiB fallback", d.bytes == N48_LUT_FALLBACK_BYTES, 0u);
    expect_u("decode: 48 pages", n48_lut_page_count(d.bytes), 48u);
    expect_u("decode: page_count of the fallback is 12", n48_lut_page_count(N48_LUT_FALLBACK_BYTES), 12u);

    // The WIDTH_LO positive controls: the RAW field decode yields the measured geometry of two real surfaces, whose
    // records are 8_8_8_8 and so may not pass the LUT's own 32_FLOAT gate. This is what pins the WIDTH_LO term.
    uint32_t rec[N48_LUT_RECORD_DWORDS]; n48_lut_fields f;
    n48_lut_fields_of(kMidRecord, &f);
    expect_u("control: the composited surface is 1920 wide", f.elements, 1920u);
    expect_u("control: the composited surface is 1080 high", f.height, 1080u);
    expect_u("control: the composited surface is one 2D slice", f.slices, 1u);
    expect_u("control: its VA is 0x400800000", f.va, 0x400800000ull);
    expect_u("control: it is TYPE 9 (2D)", f.type, 9u);
    n48_lut_fields_of(kWinRecord, &f);
    expect_u("control: f2's image is 28 wide", f.elements, 28u);
    expect_u("control: f2's image is 40 high", f.height, 40u);
    expect_u("control: f2's image VA is 0x400006000", f.va, 0x400006000ull);
    n48_lut_fields_of(kLutRecord, &f);
    expect_u("control: the LUT record is WIDTH_LO 3 | WIDTH_HI 4095 -> 16384", f.elements, 16384u);

    // Refusals: each is one named miss, and each leaves the descriptor a defined zero.
    n48_lut_desc z;
    expect_u("refuse: a null record", n48_lut_decode_record(nullptr, &z), N48_LUT_ABSENT);
    expect_u("refuse: a null descriptor", n48_lut_decode_record(kLutRecord, nullptr), N48_LUT_ABSENT);
    const uint32_t zero[N48_LUT_RECORD_DWORDS] = { 0, 0, 0, 0, 0, 0, 0, 0 };
    expect_u("refuse: an all-zero record (nobody read the heap)", n48_lut_decode_record(zero, &z), N48_LUT_ABSENT);
    std::memcpy(rec, kLutRecord, sizeof rec); rec[1] = (rec[1] & ~(0x1ffu << 20)) | (56u << 20);   // 8_8_8_8_UNORM
    expect_u("refuse: not 32_FLOAT", n48_lut_decode_record(rec, &z), N48_LUT_FMT);
    std::memcpy(rec, kLutRecord, sizeof rec); rec[3] = (rec[3] & ~(0xfu << 28)) | (0u << 28);       // BUFFER
    expect_u("refuse: not an array type", n48_lut_decode_record(rec, &z), N48_LUT_TYPE);
    std::memcpy(rec, kLutRecord, sizeof rec); rec[2] |= (5u << 14);                                  // HEIGHT 6
    expect_u("refuse: a taller surface (a different address equation)", n48_lut_decode_record(rec, &z), N48_LUT_SHAPE);
    std::memcpy(rec, kLutRecord, sizeof rec); rec[4] = N48_LUT_MAX_SLICES;                           // DEPTH = MAX
    expect_u("refuse: too many slices", n48_lut_decode_record(rec, &z), N48_LUT_SHAPE);
    expect_u("refuse: the reason names are distinct", (uint64_t)(std::strcmp(n48_lut_reason_name(N48_LUT_ABSENT),
                                                                              n48_lut_reason_name(N48_LUT_FMT)) != 0), 1u);
    expect_u("refuse: the record/producer disagreement has its own named reason (0.0.553: + NOT_1D, INVA appended)", N48_LUT_REASONS, 12u);
    expect_u("refuse: 0.0.553 appended, never inserted: STALE is still 9, NOT_1D 10, INVA 11",
             N48_LUT_STALE == 9u && N48_LUT_NOT_1D == 10u && N48_LUT_INVA == 11u ? 1u : 0u, 1u);
    expect_u("refuse: 0.0.553 the two new reasons read back as distinct names, not '?'",
             std::strcmp(n48_lut_reason_name(N48_LUT_NOT_1D), "?") != 0 && std::strcmp(n48_lut_reason_name(N48_LUT_INVA), "?") != 0 &&
             std::strcmp(n48_lut_reason_name(N48_LUT_NOT_1D), n48_lut_reason_name(N48_LUT_INVA)) != 0 ? 1u : 0u, 1u);
    expect_u("refuse: and it reads back as a name, not '?'",
             (uint64_t)(std::strcmp(n48_lut_reason_name(N48_LUT_MISMATCH), "?") != 0), 1u);
    // 0.0.402: the four APPENDED reasons each read back as a distinct, non-'?' name, so a report tail
    // that prints a reason can never print "?" and no two reasons are confused in a log.
    {
        const uint32_t rs[4] = { N48_LUT_INSTRUMENT, N48_LUT_PAGE, N48_LUT_HOST, N48_LUT_STALE };
        uint32_t distinct = 1u;
        for (uint32_t a = 0u; a < 4u; a++) {
            if (std::strcmp(n48_lut_reason_name(rs[a]), "?") == 0) distinct = 0u;
            for (uint32_t b = a + 1u; b < 4u; b++)
                if (std::strcmp(n48_lut_reason_name(rs[a]), n48_lut_reason_name(rs[b])) == 0) distinct = 0u;
        }
        expect_u("refuse: the four 0.0.402 reasons are distinct and named", distinct, 1u);
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// 1b. 0.0.402 — THE INSTRUMENT'S ONE SHAPE
// ---------------------------------------------------------------------------------------------------------------------
static void instrument_checks()
{
    n48_lut_desc d;
    expect_u("instrument: the real record passes the instrument's shape gate",
             n48_lut_decode_record(kLutRecord, &d) == N48_LUT_OK ? n48_lut_instrument_ok(&d) : 0xFFFFFFFFu, N48_LUT_OK);
    expect_u("instrument: a null descriptor refuses", n48_lut_instrument_ok(nullptr), N48_LUT_ABSENT);
    // A page-misaligned base is refused: the ramp is written page by page from the base, so element 0 would land off-page.
    n48_lut_desc m = d; m.va |= 0x800ull;
    expect_u("instrument: a VA that is not page aligned is refused", n48_lut_instrument_ok(&m), N48_LUT_INSTRUMENT);
    //'s SUSPECTED 4096-element shape decodes green but is NOT this instrument's record: the gate must refuse it.
    n48_lut_desc f = d; f.elements = N48_LUT_FALLBACK_ELEMENTS; f.bytes = N48_LUT_FALLBACK_BYTES;
    expect_u("instrument: the 4096-element fallback shape decodes but is refused", n48_lut_instrument_ok(&f), N48_LUT_INSTRUMENT);
    n48_lut_desc s = d; s.slices = 1u; s.bytes = N48_LUT_INSTRUMENT_ELEMENTS * 4ull;
    expect_u("instrument: one slice is refused (the LUT is three)", n48_lut_instrument_ok(&s), N48_LUT_INSTRUMENT);
    n48_lut_desc b = d; b.bytes = N48_LUT_INSTRUMENT_BYTES + 4ull;
    expect_u("instrument: an extent that disagrees with 3x16384x4 is refused", n48_lut_instrument_ok(&b), N48_LUT_INSTRUMENT);
    n48_lut_desc h = d; h.height = 2u;
    expect_u("instrument: a taller surface is refused", n48_lut_instrument_ok(&h), N48_LUT_INSTRUMENT);
}

// ---------------------------------------------------------------------------------------------------------------------
// 1c. 0.0.402 — THE PLANE SHAPE the learn is gated on
// ---------------------------------------------------------------------------------------------------------------------
static void plane_shape_checks()
{
    const uint32_t tl[2] = { 27u, 0u };   // slot 1 tiled, slot 4 linear
    const uint32_t lt[2] = { 0u, 27u };   // the same two surfaces, the list the other way round
    const uint32_t tt[2] = { 27u, 22u };
    const uint32_t ll[2] = { 0u, 0u };
    const uint32_t one[1] = { 0u };
    const uint32_t three[3] = { 27u, 0u, 0u };
    expect_u("plane: one tiled + one linear is the shape", n48_lut_plane_shape(tl, 2u), 1u);
    expect_u("plane: the list order is NOT assumed", n48_lut_plane_shape(lt, 2u), 1u);
    expect_u("plane: two tiled images is not", n48_lut_plane_shape(tt, 2u), 0u);
    expect_u("plane: two linear images is not", n48_lut_plane_shape(ll, 2u), 0u);
    expect_u("plane: one input is not", n48_lut_plane_shape(one, 1u), 0u);
    expect_u("plane: three inputs is not", n48_lut_plane_shape(three, 3u), 0u);
    expect_u("plane: a null list is not", n48_lut_plane_shape(nullptr, 2u), 0u);
}

// ---------------------------------------------------------------------------------------------------------------------
// 2. the identity ramp: endpoints and the whole range against the host's own correctly-rounded quotient
// ---------------------------------------------------------------------------------------------------------------------
static void ramp_checks()
{
    // Endpoints, per slice (the ramp is the same in every slice; the slice split is checked below).
    const uint32_t ns[6] = { 2u, 3u, 16u, 4096u, 16384u, 7u };
    for (uint32_t k = 0; k < 6u; k++) {
        const uint32_t n = ns[k];
        char lbl[96];
        std::snprintf(lbl, sizeof lbl, "ramp n=%u: first element is +0.0f", n);
        expect_u(lbl, n48_lut_ramp_dw(0u, n), 0x00000000u);
        std::snprintf(lbl, sizeof lbl, "ramp n=%u: last element is exactly 1.0f", n);
        expect_u(lbl, n48_lut_ramp_dw(n - 1u, n), 0x3F800000u);
        std::snprintf(lbl, sizeof lbl, "ramp n=%u: the middle element is the middle quotient", n);
        expect_u(lbl, n48_lut_ramp_dw(n / 2u, n), host_float_bits(n / 2u, n));
        // The FULL range: the FPU-free integer routine must equal the host float for EVERY element.
        uint32_t bad = 0u;
        for (uint32_t i = 0; i < n; i++) if (n48_lut_ramp_dw(i, n) != host_float_bits(i, n)) bad++;
        std::snprintf(lbl, sizeof lbl, "ramp n=%u: every element equals i/(n-1) (mismatches)", n);
        expect_u(lbl, bad, 0u);
    }
    // n <= 1 and out-of-range i are defined and cannot wrap.
    expect_u("ramp n=0 is +0.0f", n48_lut_ramp_dw(0u, 0u), 0x00000000u);
    expect_u("ramp n=1 is +0.0f", n48_lut_ramp_dw(0u, 1u), 0x00000000u);
    expect_u("ramp i past the end clamps to 1.0f", n48_lut_ramp_dw(999u, 10u), 0x3F800000u);
    // A few named values, so a reviewer can read the ramp without running it: 0.5, 1/3, 2/3.
    expect_u("ramp: 1/2 is 0.5f", n48_lut_ramp_dw(1u, 3u), 0x3F000000u);
    expect_u("ramp: 1/3 is 0.33333334f", n48_lut_ramp_dw(1u, 4u), 0x3EAAAAABu);
    expect_u("ramp: 2/3 is 0.6666667f", n48_lut_ramp_dw(2u, 4u), 0x3F2AAAABu);
}

// ---------------------------------------------------------------------------------------------------------------------
// 3. page bookkeeping and the (slice, element) split
// ---------------------------------------------------------------------------------------------------------------------
static void page_checks()
{
    const uint64_t bytes = 196608ull;
    expect_u("pages: 192 KiB is 48 pages", n48_lut_page_count(bytes), 48u);
    expect_u("pages: every page of a page-aligned extent is 4096", 
             (n48_lut_page_bytes(0u, bytes) == 4096u && n48_lut_page_bytes(47u, bytes) == 4096u &&
              n48_lut_page_bytes(1u, bytes) == 4096u) ? 1u : 0u, 1u);
    expect_u("pages: past the last page is 0 bytes", n48_lut_page_bytes(48u, bytes), 0u);
    expect_u("pages: a non-aligned extent (4097) is 2 pages", n48_lut_page_count(4097ull), 2u);
    expect_u("pages: its last page holds 1 byte", n48_lut_page_bytes(1u, 4097ull), 1u);
    expect_u("pages: 0 bytes is 0 pages (refuse)", n48_lut_page_count(0ull), 0u);
    expect_u("pages: 1 byte is 1 page", n48_lut_page_count(1ull), 1u);

    n48_lut_desc d;
    (void)n48_lut_decode_record(kLutRecord, &d);
    uint32_t s = 0u, i = 0u;
    expect_u("split: dword 0 -> (slice 0, elem 0)", n48_lut_split(&d, 0u, &s, &i) ? (s == 0u && i == 0u ? 1u : 0u) : 0u, 1u);
    expect_u("split: dword 16383 -> (slice 0, elem 16383)", n48_lut_split(&d, 16383u, &s, &i) ? (s == 0u && i == 16383u ? 1u : 0u) : 0u, 1u);
    expect_u("split: dword 16384 -> (slice 1, elem 0)", n48_lut_split(&d, 16384u, &s, &i) ? (s == 1u && i == 0u ? 1u : 0u) : 0u, 1u);
    expect_u("split: the last dword -> (slice 2, elem 16383)", n48_lut_split(&d, 3u * 16384u - 1u, &s, &i) ? (s == 2u && i == 16383u ? 1u : 0u) : 0u, 1u);
    expect_u("split: one past the end refuses", n48_lut_split(&d, 3u * 16384u, &s, &i), 0u);
    expect_u("split: a null descriptor refuses", n48_lut_split(nullptr, 0u, &s, &i), 0u);
}

// ---------------------------------------------------------------------------------------------------------------------
// 3b. 0.0.402 — THE BATCH GENERATOR against the WHOLE-RAMP reference, for EVERY page
// ---------------------------------------------------------------------------------------------------------------------
static void batch_checks()
{
    n48_lut_desc d;
    expect_u("batch: the real record decodes", n48_lut_decode_record(kLutRecord, &d), N48_LUT_OK);
    const uint32_t np = n48_lut_page_count(d.bytes);
    expect_u("batch: the extent is 48 pages", np, 48u);
    // The whole-ramp reference: every dword of the surface, element by element, from the SAME split+ramp the generator uses.
    static uint32_t ref[N48_LUT_INSTRUMENT_ELEMENTS * N48_LUT_INSTRUMENT_SLICES];
    for (uint32_t g = 0u; g < N48_LUT_INSTRUMENT_ELEMENTS * N48_LUT_INSTRUMENT_SLICES; g++) {
        uint32_t sl = 0u, i = 0u;
        (void)n48_lut_split(&d, g, &sl, &i);
        ref[g] = n48_lut_ramp_dw(i, d.elements);
    }
    uint32_t mismatches = 0u, batches = 0u, refused = 0u;
    for (uint32_t p = 0u; p < np; p++) {
        const uint32_t pdw = n48_lut_page_bytes(p, d.bytes) / 4u;
        for (uint32_t off = 0u; off < pdw; off += N48_LUT_BATCH_DWORDS) {
            const uint32_t take = (pdw - off) < N48_LUT_BATCH_DWORDS ? (pdw - off) : N48_LUT_BATCH_DWORDS;
            uint32_t out[N48_LUT_BATCH_DWORDS];
            if (!n48_lut_batch_page(&d, p, off, take, out)) { refused++; continue; }
            batches++;
            const uint64_t base = (uint64_t)p * (uint64_t)pdw + (uint64_t)off;
            for (uint32_t j = 0u; j < take; j++) if (out[j] != ref[base + (uint64_t)j]) mismatches++;
        }
    }
    expect_u("batch: every page generated with no refusal", refused, 0u);
    expect_u("batch: 48 pages x 16 batches = 768 batches", batches, 768u);
    expect_u("batch: the whole ramp matches (mismatches)", mismatches, 0u);
    // Fail-closed edges: a take over 64, a run past a page, a run past the surface, a null descriptor.
    uint32_t out[N48_LUT_BATCH_DWORDS];
    expect_u("batch: take 0 refuses", n48_lut_batch_page(&d, 0u, 0u, 0u, out), 0u);
    expect_u("batch: take 65 refuses", n48_lut_batch_page(&d, 0u, 0u, 65u, out), 0u);
    expect_u("batch: a run past the page's 1024 dwords refuses", n48_lut_batch_page(&d, 0u, 1020u, 64u, out), 0u);
    expect_u("batch: a run past the last page refuses", n48_lut_batch_page(&d, 48u, 0u, 64u, out), 0u);
    expect_u("batch: a null descriptor refuses", n48_lut_batch_page(nullptr, 0u, 0u, 64u, out), 0u);
}

// ---------------------------------------------------------------------------------------------------------------------
// 4. the ONE report line's width, at the widest arguments the kext can render
// ---------------------------------------------------------------------------------------------------------------------
static void report_width_checks()
{
    char body[1024];
    // The TRUE worst case: the longest state/change strings, every numeric argument at its type's ceiling, and the
    // longest tail strings. If this fits, nothing the kext can print this line with can overflow it.
    const int n = std::snprintf(body, sizeof body, N48_LUTFILL_FMT,
                                "OFF (default)", "`gfxneuter 32` REFUSED it, unchanged",
                                (unsigned long long)0xffffffffffffffffull, 0xffffffffu, 0xffffffffu,
                                (unsigned long long)0xffffffffffffffffull, 0xffffffffu,
                                0xffffffffu, 0xffffffffu, 0xffffffffu, 0xffffffffu, 0xffffffffu,
                                (unsigned long long)0xffffffffffffffffull,
                                "REFUSED by the destination guard", "not run", "record-absent-or-zero");
    if (n < 0) { gFail++; gRun++; std::printf("FAIL  lutfill report line did not render\n"); return; }
    expect_u("line: the worst-case report body is under 480 B", (uint64_t)n < N48_LUTFILL_BODY_CAP, 1u);
    expect_u("line: ... and under the logger's 491 B body cap", (uint64_t)n <= (uint64_t)N48_LOG_CAP_BODY, 1u);
    // 0.0.402: the state word and the change word are SEPARATED. Through 0.0.401 the format was `is %s%s.`,
    // so the two ran together; the fixed line must carry " - " between them, and the old form must NOT read as fixed.
    expect_u("line: H7 the state and the change word are separated by \" - \"",
             (uint64_t)(std::strstr(body, "OFF (default) - `gfxneuter 32` REFUSED it, unchanged.") != nullptr), 1u);
    {
        char oldsep[64];
        std::snprintf(oldsep, sizeof oldsep, "is %s%s.", "ON", "CHANGEWORD");   // the 0.0.401 shape
        expect_u("line: H7 and the separator-less form is distinguishable from the fixed line",
                 (uint64_t)(std::strstr(oldsep, "ON - CHANGEWORD") == nullptr), 1u);
    }
    if (!gQuiet) std::printf("      lutfill worst-case body width: %d B (cap %u)\n", n, (unsigned)N48_LUTFILL_BODY_CAP);
}

// ---------------------------------------------------------------------------------------------------------------------
// 5. planted defects in COPIES of the pure code. Each must be caught by the checks above.
// ---------------------------------------------------------------------------------------------------------------------
static uint32_t mut_decode_no_wlo(const uint32_t rec[N48_LUT_RECORD_DWORDS], n48_lut_desc *d)   //'s SUSPECTED read
{
    std::memset(d, 0, sizeof *d);
    if (!rec || !rec[0]) return N48_LUT_ABSENT;
    if (((rec[1] >> 20) & 0x1ffu) != N48_LUT_FMT_32F) return N48_LUT_FMT;
    d->elements = (rec[2] & 0xfffu) + 1u;     // WIDTH_HI only: the 4x error
    d->height = ((rec[2] >> 14) & 0x3fffu) + 1u;
    d->slices = (rec[4] & 0x1fffu) + 1u;
    d->bytes = (uint64_t)d->elements * d->height * d->slices * 4u;
    return N48_LUT_OK;
}
static uint32_t mut_ramp_truncate(uint32_t i, uint32_t n)          // no round-to-nearest: the middle is off by one ulp
{
    if (n <= 1u || i == 0u) return 0x00000000u;
    const uint32_t dd = n - 1u;
    if (i >= dd) return 0x3F800000u;
    uint64_t num = i, den = dd; int k = 0;
    while (num < den) { num <<= 1; k++; }
    const uint64_t q = (num << 23) / den;   // truncated, never rounded
    return (uint32_t)((127 - k) << 23) | (uint32_t)(q & 0x7FFFFFu);
}
static uint32_t mut_page_floor(uint64_t bytes)                     // rounds down instead of up
{
    if (bytes == 0ull) return 0u;
    return (uint32_t)(bytes / N48_LUT_PAGE_BYTES);
}
static uint32_t mut_split_by_height(const n48_lut_desc *d, uint32_t g)   // splits into rows, not slices
{
    if (!d || d->height == 0u) return 0u;
    return ((uint64_t)g >= (uint64_t)d->elements * d->height) ? 0u : 1u;
}
/* 0.0.402 mutants: the pure functions got wrong in one clause each. */
static uint32_t mut_instrument_no_align(const n48_lut_desc *d)    // H4: the page-alignment clause is dropped
{
    if (!d) return N48_LUT_ABSENT;
    if (d->elements != N48_LUT_INSTRUMENT_ELEMENTS) return N48_LUT_INSTRUMENT;
    if (d->height != 1u) return N48_LUT_INSTRUMENT;
    if (d->slices != N48_LUT_INSTRUMENT_SLICES) return N48_LUT_INSTRUMENT;
    if (d->bytes != N48_LUT_INSTRUMENT_BYTES) return N48_LUT_INSTRUMENT;
    return N48_LUT_OK;
}
static uint32_t mut_instrument_any_elems(const n48_lut_desc *d)   // H4: any element count is accepted
{
    if (!d) return N48_LUT_ABSENT;
    if ((d->va & 0xfffull) != 0ull) return N48_LUT_INSTRUMENT;
    if (d->height != 1u) return N48_LUT_INSTRUMENT;
    if (d->slices != N48_LUT_INSTRUMENT_SLICES) return N48_LUT_INSTRUMENT;
    return N48_LUT_OK;   // elements/bytes unchecked: the 4096 fallback would pass
}
static uint32_t mut_plane_any_two(const uint32_t *mode, uint32_t n)   // H2: ANY two inputs read as the plane shape
{
    if (!mode || n != 2u) return 0u;
    return 1u;
}
static uint32_t mut_batch_off_ignored(const n48_lut_desc *d, uint32_t page, uint32_t off,
                                      uint32_t take, uint32_t out[N48_LUT_BATCH_DWORDS])   // H1: `off` ignored
{
    (void)off;   // the batch is generated from the page's START whatever the offset - the page-at-a-time shape in disguise
    return n48_lut_batch_page(d, page, 0u, take, out);
}


// ---------------------------------------------------------------------------------------------------------------------
// 111. build 0.0.553 (; gfx_lutidx111.h): THE LEARN READS THE HEAP ENTRY THE DRAW NAMES. Fixtures are the MEASURED
// records: run11ar (RUN AX, hardware cursor) `stale103: box AB ... pgm 0x53d3d36bb9 tex 1 heap 3 base 0x402000000 ... T# 04020000
// c1600000 80000fff c0000204 00000002 00400000 00000000 00000000` and `tex 0 heap 1 base 0x400100000 ... sw 27 ... T# 04001000
// c3200000 8167c27f 99b00f2e 00000000 00400000 00000200 00000000`; run11aq (RUN AW, software cursor) `tex 1 heap 4 base
// 0x401140000 ... T# 04011400 c1600000 80000fff c0000204 00000002 00400000 00000000 00000000`; run11av (RUN BB, hardware cursor)
// the heap page `vmpage: pg[018] 04020200 c1600000 80000fff c0000204 00000002 00400000 00000000 00000000` (entry 3) and `pg[020]
// 04022000 c3800000 8167c27f 99b00f2e 00000000 00400000 00000200 00000000` (entry 4: the fmt 56 plane record lutretry107 refused
// 11 times). The model below composes the SAME pure calls, in the SAME order, as the hook's learn site + gfxsrc_lut_learn's record
// steps (li111_pick -> n48_li_pick; li111_learn_off -> n48_li_learn_off; decode; instrument_ok; li111_accept -> n48_li_accept_sel;
// the producer cross-check); tests/gfx_commit_test.cpp's 111 section pins that composition in the hook's source.
// ---------------------------------------------------------------------------------------------------------------------
namespace li {
struct Heap { uint32_t e[64][8]; };
struct Draw { uint32_t gp, in_n, in_over, in_mode[4]; uint64_t in_va[4]; uint32_t in_idx[4]; };
enum { R_SKIPPED = 100u };
struct Out { uint32_t why; uint64_t off, va; uint32_t read, kicked, use; };
static const uint32_t kAX3[8] = { 0x04020000u, 0xc1600000u, 0x80000fffu, 0xc0000204u, 0x00000002u, 0x00400000u, 0u, 0u };
static const uint32_t kAX1[8] = { 0x04001000u, 0xc3200000u, 0x8167c27fu, 0x99b00f2eu, 0x00000000u, 0x00400000u, 0x00000200u, 0u };
static const uint32_t kAW4[8] = { 0x04011400u, 0xc1600000u, 0x80000fffu, 0xc0000204u, 0x00000002u, 0x00400000u, 0u, 0u };
static const uint32_t kBB3[8] = { 0x04020200u, 0xc1600000u, 0x80000fffu, 0xc0000204u, 0x00000002u, 0x00400000u, 0u, 0u };
static const uint32_t kBB4[8] = { 0x04022000u, 0xc3800000u, 0x8167c27fu, 0x99b00f2eu, 0x00000000u, 0x00400000u, 0x00000200u, 0u };
static const uint32_t kBB2[8] = { 0x04011000u, 0xc3800000u, 0x8167c27fu, 0x99b00f2eu, 0x00000000u, 0x00400000u, 0x00000200u, 0u };
static void put(Heap &h, uint32_t i, const uint32_t *r) { for (uint32_t k = 0u; k < 8u; k++) h.e[i][k] = r[k]; }
// The hook's order: the pick (only when 111 is not OFF); SKIP -> no read, no learn; else read at learn_off, decode, shape, accept,
// the producer cross-check. `kicked` = the learn reached its page walk (every record step green): the only path to a write.
static Out learn(uint32_t mode, const Heap &h, const Draw &d, uint64_t prodVa)
{
    Out o {}; n48_li_sel sel {};
    if (mode != N48_LI_OFF) {
        const uint32_t act = n48_li_pick(mode, d.gp, d.in_n, d.in_over, d.in_mode, d.in_va, d.in_idx, &sel);
        o.use = sel.use;
        if (act == N48_LI_ACT_SKIP) { o.why = R_SKIPPED; o.off = ~0ull; return o; }
        if (act != N48_LI_ACT_USE) sel.use = 0u;
    }
    o.off = n48_li_learn_off(&sel);
    const uint64_t e = o.off / 32u;
    uint32_t rec[8] = { 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u };
    if ((o.off & 31u) == 0u && e < 64u) for (uint32_t k = 0u; k < 8u; k++) rec[k] = h.e[e][k];
    o.read = 1u;
    n48_lut_desc dd {};
    uint32_t why = n48_lut_decode_record(rec, &dd);
    if (why == N48_LUT_OK) why = n48_lut_instrument_ok(&dd);
    if (why == N48_LUT_OK) why = n48_li_accept_sel(&sel, rec, &dd);
    if (why == N48_LUT_OK && prodVa && prodVa != dd.va) why = N48_LUT_MISMATCH;
    o.why = why;
    if (why == N48_LUT_OK) { o.va = dd.va; o.kicked = 1u; }
    return o;
}
static Heap axHeap() { Heap h {}; put(h, 1u, kAX1); put(h, 3u, kAX3); put(h, 4u, kBB4); return h; }   // AX entry 4: not logged; BB's plane record stands in (: AX's learn refused not-32_FLOAT)
static Heap awHeap() { Heap h {}; put(h, 1u, kAX1); put(h, 4u, kAW4); return h; }
static Heap bbHeap() { Heap h {}; put(h, 2u, kBB2); put(h, 3u, kBB3); put(h, 4u, kBB4); return h; }
static Draw draw(uint32_t i0, uint64_t v0, uint32_t i1, uint64_t v1)
{
    Draw d {}; d.gp = 1u; d.in_n = 2u; d.in_over = 0u; d.in_mode[0] = 3u; d.in_mode[1] = 0u;
    d.in_va[0] = v0; d.in_va[1] = v1; d.in_idx[0] = i0; d.in_idx[1] = i1; return d;
}
} // namespace li

static void lutidx111_checks()
{
    using namespace li;
    const Heap ax = axHeap(), aw = awHeap(), bb = bbHeap();
    const Draw dAX = draw(1u, 0x400100000ull, 3u, 0x402000000ull);
    const Draw dAW = draw(1u, 0x400100000ull, 4u, 0x401140000ull);
    const Draw dBB = draw(2u, 0x401100000ull, 3u, 0x402020000ull);   // input 0's index is not logged in run11av: 2 stands in
    // the arithmetic
    expect_u("111 slot_off: entry 4 is 0x80 == N48_LUT_SLOT4_OFF (the old read)", n48_li_slot_off(4u) == 0x80u && N48_LUT_SLOT4_OFF == 0x80u ? 1u : 0u, 1u);
    expect_u("111 slot_off: entry 3 is 0x60 (run11ar's residency copy `source+0x60 04020000 c1600000`)", n48_li_slot_off(3u), 0x60u);
    expect_u("111 slot_off: sext32(idx << 5) as the translator's d_tbl_off (0xffffffff -> -32)", n48_li_slot_off(0xFFFFFFFFu), 0xFFFFFFFFFFFFFFE0ull);
    expect_u("111 the verb's M: 1 ON (367), 2 OFF (623), 3 SHADOW (879), others refused",
             n48_li_mode_of_m(1u) == N48_LI_ON && n48_li_mode_of_m(2u) == N48_LI_OFF && n48_li_mode_of_m(3u) == N48_LI_SHADOW &&
             n48_li_mode_of_m(4u) == N48_LI_MODES && n48_li_mode_of_m(0u) == N48_LI_MODES && (111u | 1u << 8) == 367u &&
             (111u | 2u << 8) == 623u && (111u | 3u << 8) == 879u && N48_LI_OFF == 0u ? 1u : 0u, 1u);
    expect_u("111 the combinedLUT is input q == 1 (GPUPass row {83, 0xd3d36bb9} tex {4, 6}: s6)",
             N48_LI_Q == 1u && N48_LI_GP_NDW == 83u && N48_LI_GP_FNV == 0xd3d36bb9u ? 1u : 0u, 1u);
    // T1 AX idx 3 learned (ON); OFF and SHADOW read entry 4 and learn nothing (the black planes)
    { const Out on = learn(N48_LI_ON, ax, dAX, 0ull), off = learn(N48_LI_OFF, ax, dAX, 0ull), sh = learn(N48_LI_SHADOW, ax, dAX, 0ull);
      expect_u("111 T1 AX ON: reads entry 3 (0x60) and LEARNS 0x402000000", on.why == N48_LUT_OK && on.off == 0x60u && on.va == 0x402000000ull && on.kicked ? 1u : 0u, 1u);
      expect_u("111 T1 AX OFF: reads entry 4 (0x80) and refuses not-32_FLOAT (0.0.552's black planes)", off.why == N48_LUT_FMT && off.off == 0x80u && !off.kicked ? 1u : 0u, 1u);
      expect_u("111 T1 AX SHADOW: exactly OFF's read and result", sh.why == off.why && sh.off == off.off && sh.va == off.va && sh.kicked == off.kicked && !sh.use ? 1u : 0u, 1u); }
    // T2 AW idx 4 learned and identical to OFF
    { const Out on = learn(N48_LI_ON, aw, dAW, 0ull), off = learn(N48_LI_OFF, aw, dAW, 0ull);
      expect_u("111 T2 AW ON: entry 4 learned, 0x401140000", on.why == N48_LUT_OK && on.va == 0x401140000ull && on.kicked ? 1u : 0u, 1u);
      expect_u("111 T2 AW ON == OFF: the same offset (0x80), the same VA, the same kick", on.off == off.off && on.off == 0x80u && on.va == off.va &&
               on.kicked == off.kicked && on.why == off.why ? 1u : 0u, 1u);
      n48_lut_desc a {}, b {}; (void)n48_lut_decode_record(aw.e[4], &a); (void)n48_lut_decode_record(kAW4, &b);
      expect_u("111 T2 AW the record read is byte-for-byte the old slot-4 record (16384 x 3, 196608 B)",
               a.va == b.va && a.elements == 16384u && a.slices == 3u && a.bytes == 196608ull ? 1u : 0u, 1u);
      const Out onP = learn(N48_LI_ON, aw, dAW, 0x401140000ull), offP = learn(N48_LI_OFF, aw, dAW, 0x401140000ull);
      expect_u("111 T2 AW with the producer seen at the LUT: ON == OFF, learned", onP.why == N48_LUT_OK && offP.why == N48_LUT_OK && onP.va == offP.va ? 1u : 0u, 1u); }
    // T3 BB: the entry-4 PLANE record is never learned from entry 4
    { const Out on = learn(N48_LI_ON, bb, dBB, 0ull), off = learn(N48_LI_OFF, bb, dBB, 0ull);
      expect_u("111 T3 BB ON: reads entry 3 (0x60), learns 0x402020000, never 0x402200000", on.why == N48_LUT_OK && on.off == 0x60u &&
               on.va == 0x402020000ull && on.va != 0x402200000ull ? 1u : 0u, 1u);
      expect_u("111 T3 BB OFF: entry 4 is the fmt 56 plane record (refused not-32_FLOAT, as lutretry107 logged 11 times)", off.why == N48_LUT_FMT && off.off == 0x80u ? 1u : 0u, 1u);
      Draw d4 = dBB; d4.in_idx[1] = 4u;   // a draw naming entry 4 while its LUT input is the entry-3 LUT's VA
      const Out o4 = learn(N48_LI_ON, bb, d4, 0ull);
      expect_u("111 T3 BB a draw naming entry 4: the plane record there is refused, nothing learned", o4.why != N48_LUT_OK && !o4.kicked && o4.va == 0ull ? 1u : 0u, 1u);
      Draw dt = dBB; dt.in_idx[1] = 4u; dt.in_mode[1] = 3u; dt.in_va[1] = 0x402200000ull;   // the translator's own view of entry 4: TILED
      const Out ot = learn(N48_LI_ON, bb, dt, 0ull);
      expect_u("111 T3 BB input 1 tiled (the plane record): ON SKIPS - no read at all, never entry 4", ot.why == R_SKIPPED && !ot.read && !ot.kicked ? 1u : 0u, 1u);
      for (uint32_t m = 0u; m < 3u; m++) {
          const Out x = learn(m, bb, dBB, 0ull);
          expect_u("111 T3 BB in no mode is 0x402200000 learned", x.va != 0x402200000ull ? 1u : 0u, 1u);
      } }
    // T4 VA mismatch refused
    { Draw d = dAX; d.in_va[1] = 0x401140000ull;
      const Out o = learn(N48_LI_ON, ax, d, 0ull);
      expect_u("111 T4 AX entry 3 with in_va[1] != its record's VA: REFUSED INVA, nothing learned", o.why == N48_LUT_INVA && !o.kicked ? 1u : 0u, 1u);
      Draw e = dAW; e.in_va[1] = 0x402000000ull;
      const Out p = learn(N48_LI_ON, aw, e, 0ull);
      expect_u("111 T4 AW entry 4 with another VA named: REFUSED INVA (ON checks entry 4 too)", p.why == N48_LUT_INVA && !p.kicked ? 1u : 0u, 1u);
      uint32_t r[8]; std::memcpy(r, kAX3, sizeof r); r[3] = (r[3] & ~(0xfu << 28)) | (N48_LUT_TYPE_2D_ARRAY << 28);
      Heap h = ax; put(h, 3u, r);
      const Out q = learn(N48_LI_ON, h, dAX, 0ull), qo = learn(N48_LI_OFF, h, draw(1u, 0x400100000ull, 4u, 0x402000000ull), 0ull);
      expect_u("111 T4 a 2D_ARRAY record at the named entry: ON refuses NOT_1D", q.why == N48_LUT_NOT_1D && !q.kicked ? 1u : 0u, 1u);
      (void)qo;
      const Out pr = learn(N48_LI_ON, ax, dAX, 0x401140000ull);
      expect_u("111 T4 the producer cross-check still refuses under ON", pr.why == N48_LUT_MISMATCH && !pr.kicked ? 1u : 0u, 1u); }
    // T5 SHADOW never writes: never selects, always reads entry 4, equals OFF on every fixture and variant
    { const Heap hs[3] = { ax, aw, bb }; const Draw ds[3] = { dAX, dAW, dBB };
      uint32_t bad = 0u;
      for (uint32_t k = 0u; k < 3u; k++)
          for (uint32_t v = 0u; v < 6u; v++) {
              Draw d = ds[k];
              if (v == 1u) d.gp = 0u; if (v == 2u) d.in_over = 1u; if (v == 3u) d.in_mode[1] = 3u; if (v == 4u) d.in_va[1] = 0ull;
              if (v == 5u) d.in_idx[1] = 7u;
              const Out s = learn(N48_LI_SHADOW, hs[k], d, 0ull), o = learn(N48_LI_OFF, hs[k], d, 0ull);
              n48_li_sel sel {}; (void)n48_li_pick(N48_LI_SHADOW, d.gp, d.in_n, d.in_over, d.in_mode, d.in_va, d.in_idx, &sel);
              if (s.use || sel.use || s.off != 0x80u || s.why != o.why || s.va != o.va || s.kicked != o.kicked ||
                  n48_li_learn_off(&sel) != 0x80u || n48_li_accept_sel(&sel, kBB4, nullptr) != N48_LUT_OK) bad++;
          }
      expect_u("111 T5 SHADOW never selects (use 0), always reads entry 4 and learns exactly what OFF learns (18 cases)", bad, 0u);
      n48_li_sel sel {}; const uint32_t act = n48_li_pick(N48_LI_SHADOW, 1u, 2u, 0u, dAX.in_mode, dAX.in_va, dAX.in_idx, &sel);
      expect_u("111 T5 SHADOW still fills idx/off/va for its peek (entry 3, 0x60) but LEGACY", act == N48_LI_ACT_LEGACY && sel.idx == 3u &&
               sel.off == 0x60u && sel.va == 0x402000000ull && !sel.use ? 1u : 0u, 1u); }
    // T6 OFF identity: no selection, entry 4, no extra clause, whatever the facts
    { uint32_t bad = 0u;
      for (uint32_t gp = 0u; gp < 2u; gp++) for (uint32_t n = 0u; n < 5u; n++) for (uint32_t idx = 0u; idx < 9u; idx++) {
          const uint32_t md[4] = { 3u, 0u, 0u, 0u }; const uint64_t va[4] = { 1ull, 0x402000000ull, 0ull, 0ull };
          const uint32_t ix[4] = { 1u, idx, 0u, 0u };
          n48_li_sel sel {}; sel.use = 1u; sel.off = 0x60u;   // garbage in: the pick must overwrite it
          const uint32_t act = n48_li_pick(N48_LI_OFF, gp, n, 0u, md, va, ix, &sel);
          if (act != N48_LI_ACT_LEGACY || sel.use || n48_li_learn_off(&sel) != (uint64_t)N48_LUT_SLOT4_OFF || n48_li_learn_idx(&sel) != 4u ||
              n48_li_accept_sel(&sel, kBB4, nullptr) != N48_LUT_OK) bad++;
      }
      expect_u("111 T6 OFF: LEGACY, entry 4, idx 4, no extra clause over 90 fact sets", bad, 0u);
      expect_u("111 T6 a null selection reads entry 4", n48_li_learn_off(nullptr) == 0x80u && n48_li_learn_idx(nullptr) == 4u ? 1u : 0u, 1u); }
    // T7 ON's clauses each skip (never entry 4)
    { const Draw base = dAX; uint32_t bad = 0u;
      for (uint32_t v = 1u; v <= 5u; v++) {
          Draw d = base;
          if (v == 1u) d.gp = 0u; if (v == 2u) d.in_over = 1u; if (v == 3u) d.in_n = 1u; if (v == 4u) d.in_mode[1] = 2u; if (v == 5u) d.in_va[1] = 0ull;
          const Out o = learn(N48_LI_ON, ax, d, 0ull);
          if (o.why != R_SKIPPED || o.read || o.kicked) bad++;
      }
      expect_u("111 T7 ON: not GPUPass / list over / no input 1 / input 1 tiled / VA 0 -> SKIP, no read, no learn", bad, 0u);
      n48_li_sel sel {};
      expect_u("111 T7 the skip reasons are named", n48_li_pick(N48_LI_ON, 0u, 2u, 0u, base.in_mode, base.in_va, base.in_idx, &sel) == N48_LI_ACT_SKIP &&
               sel.why == N48_LI_WHY_NOT_GP && std::strcmp(n48_li_why_name(N48_LI_WHY_MODE), "?") != 0 ? 1u : 0u, 1u); }
}

int main()
{
    std::printf("== gfx_lutfill: the identity LUT's extent, ramp and report line ==\n");
    decode_checks();
    instrument_checks();
    ramp_checks();
    page_checks();
    plane_shape_checks();
    batch_checks();
    report_width_checks();
    lutidx111_checks();   // build 0.0.553
    const int realFail = gFail, realRun = gRun;
    std::printf("-- %d check(s), %d failure(s)\n", realRun, realFail);

    int caught = 0, nmut = 0;
    // Each mutant is run against the REAL checks: its output is compared with the real function's over a grid, and any
    // disagreement is a failure. The counts below are the real ones, per mutant.
    {
        gQuiet = 1;
        n48_lut_desc d; gFail = 0; gRun = 0;
        if (mut_decode_no_wlo(kLutRecord, &d) == N48_LUT_OK) { expect_u("m", d.elements, 16384u); expect_u("m", d.bytes, 196608ull); }
        const int f = gFail; nmut++; if (f) caught++;
        std::printf("mutant %-58s %s (%d of %d checks fail)\n", "M1 the decode drops WIDTH_LO (4096 / 48 KiB)", f ? "CAUGHT" : "NOT CAUGHT", f, gRun);
        gQuiet = 0;
    }
    {
        gQuiet = 1; gFail = 0; gRun = 0;
        for (uint32_t n = 2u; n <= 64u; n++) for (uint32_t i = 0u; i < n; i++) expect_u("m", mut_ramp_truncate(i, n), host_float_bits(i, n));
        const int f = gFail; nmut++; if (f) caught++;
        std::printf("mutant %-58s %s (%d of %d checks fail)\n", "M2 the ramp truncates instead of rounding", f ? "CAUGHT" : "NOT CAUGHT", f, gRun);
        gQuiet = 0;
    }
    {
        gQuiet = 1; gFail = 0; gRun = 0;
        expect_u("m", mut_page_floor(4097ull), n48_lut_page_count(4097ull));
        expect_u("m", mut_page_floor(196608ull), 48u);
        const int f = gFail; nmut++; if (f) caught++;
        std::printf("mutant %-58s %s (%d of %d checks fail)\n", "M3 page_count rounds down (loses the last page)", f ? "CAUGHT" : "NOT CAUGHT", f, gRun);
        gQuiet = 0;
    }
    {
        gQuiet = 1; gFail = 0; gRun = 0;
        n48_lut_desc d; (void)n48_lut_decode_record(kLutRecord, &d);
        for (uint32_t g = 0u; g <= 3u * 16384u; g += 997u) {
            uint32_t s = 0u, i = 0u;
            const uint32_t real = n48_lut_split(&d, g, &s, &i);
            expect_u("m", mut_split_by_height(&d, g), real);   // the row split disagrees past the first row
        }
        const int f = gFail; nmut++; if (f) caught++;
        std::printf("mutant %-58s %s (%d of %d checks fail)\n", "M4 split goes by rows, not slices", f ? "CAUGHT" : "NOT CAUGHT", f, gRun);
        gQuiet = 0;
    }
    // M5 (H4): the instrument's shape gate, both clauses, against the real gate over the same cases.
    {
        gQuiet = 1; gFail = 0; gRun = 0;
        n48_lut_desc d; (void)n48_lut_decode_record(kLutRecord, &d);
        n48_lut_desc c[5]; for (uint32_t k = 0u; k < 5u; k++) c[k] = d;
        c[1].va |= 0x800ull;                                                        // not page aligned
        c[2].elements = N48_LUT_FALLBACK_ELEMENTS; c[2].bytes = N48_LUT_FALLBACK_BYTES;   //'s 4096 shape
        c[3].slices = 1u; c[3].bytes = N48_LUT_INSTRUMENT_ELEMENTS * 4ull;          // one slice
        for (uint32_t k = 0u; k < 5u; k++) {
            expect_u("m", mut_instrument_no_align(&c[k]), n48_lut_instrument_ok(&c[k]));
            expect_u("m", mut_instrument_any_elems(&c[k]), n48_lut_instrument_ok(&c[k]));
        }
        const int f = gFail; nmut++; if (f) caught++;
        std::printf("mutant %-58s %s (%d of %d checks fail)\n", "M5 the instrument's shape gate loses a clause", f ? "CAUGHT" : "NOT CAUGHT", f, gRun);
        gQuiet = 0;
    }
    // M6 (H2): the plane shape read as "any two inputs", against the real classifier over its own cases.
    {
        gQuiet = 1; gFail = 0; gRun = 0;
        const uint32_t tl[2] = { 27u, 0u }, tt[2] = { 27u, 22u }, ll[2] = { 0u, 0u }, one[1] = { 0u }, three[3] = { 27u, 0u, 0u };
        const uint32_t *c[5] = { tl, tt, ll, one, three };
        const uint32_t n[5] = { 2u, 2u, 2u, 1u, 3u };
        for (uint32_t k = 0u; k < 5u; k++) expect_u("m", mut_plane_any_two(c[k], n[k]), n48_lut_plane_shape(c[k], n[k]));
        const int f = gFail; nmut++; if (f) caught++;
        std::printf("mutant %-58s %s (%d of %d checks fail)\n", "M6 the plane shape accepts any two inputs", f ? "CAUGHT" : "NOT CAUGHT", f, gRun);
        gQuiet = 0;
    }
    // M7 (H1): the batch generator ignoring `off`, against the real generator over every page and offset.
    {
        gQuiet = 1; gFail = 0; gRun = 0;
        n48_lut_desc d; (void)n48_lut_decode_record(kLutRecord, &d);
        const uint32_t np = n48_lut_page_count(d.bytes);
        for (uint32_t p = 0u; p < np; p++) {
            const uint32_t pdw = n48_lut_page_bytes(p, d.bytes) / 4u;
            for (uint32_t off = 0u; off < pdw; off += N48_LUT_BATCH_DWORDS) {
                const uint32_t take = (pdw - off) < N48_LUT_BATCH_DWORDS ? (pdw - off) : N48_LUT_BATCH_DWORDS;
                uint32_t a[N48_LUT_BATCH_DWORDS], b[N48_LUT_BATCH_DWORDS];
                const uint32_t ra = mut_batch_off_ignored(&d, p, off, take, a);
                const uint32_t rb = n48_lut_batch_page(&d, p, off, take, b);
                expect_u("m", ra, rb);
                if (ra && rb) for (uint32_t j = 0u; j < take; j++) expect_u("m", a[j], b[j]);
            }
        }
        const int f = gFail; nmut++; if (f) caught++;
        std::printf("mutant %-58s %s (%d of %d checks fail)\n", "M7 the batch generator ignores `off`", f ? "CAUGHT" : "NOT CAUGHT", f, gRun);
        gQuiet = 0;
    }
    std::printf("mutants caught %d/%d\n", caught, nmut);
    std::printf("%s\n", (realFail == 0 && caught == nmut) ? "gfx_lutfill: PASS" : "gfx_lutfill: FAIL");
    return (realFail || caught != nmut) ? 1 : 0;
}
