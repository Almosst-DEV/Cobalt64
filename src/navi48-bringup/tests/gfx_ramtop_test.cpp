// gfx_ramtop_test.cpp — the offline proof for gfx_ramtop.h (0.0.386). The properties under test are the project's own
// specification of the host-page range guard's bound:
//
//     1. ON A 32 GiB MACHINE WITH THE PCI HOLE, THE DERIVED TOP IS ABOVE 0x830000000, SO THE TWO HOST PAGES arm12
//        REFUSED (0x800559000 and 0x809c47000) ARE ACCEPTED.
//     2. A 64-BIT PCI APERTURE IS NEVER COUNTED AS RAM, AND A DEVICE WINDOW INSIDE THE BAND WE WOULD OTHERWISE ACCEPT
//        REFUSES THE WHOLE DERIVATION. This is the property the guard exists for and the one a naive widening breaks.
//     3. EVERY FAILURE PATH LANDS ON THE LEGACY CONSTANT 0x800000000 — an unreadable, short, corrupt or implausible
//        map cannot move the bound at all.
//     4. THE RESULT IS MONOTONE: it is floored at the legacy constant, so no page 0.0.385 accepted becomes refused.
//     5. THE PCI-HOLE HALF OF THE GUARD IS UNCHANGED, for every bound.
//
// PLANTED-DEFECT CONTROL (rule: a test no mutation can break is not testing anything). Fourteen mutants are run against
// the SAME checks and each must be CAUGHT by at least one. D1/D2/D9 are the defect the brief names — "accepts a page
// inside the 64-bit aperture" — approached three ways.
//
// HONESTY ABOUT THE MUTANTS: n48_rt_refuse's mutants are swapped in by pointer and are therefore mutations of the REAL
// function's callers. The evaluator's mutants are a single parameterised COPY of n48_rt_eval in this file, because the
// header's evaluator is one static inline function and there is no seam to swap one clause of it. The authoritative
// non-vacuity proof for the evaluator is therefore breaking the REAL header and running this test, which the run record
// reports separately.
//
//     clang++ -std=c++17 -Wall -Wextra -Werror -O1 -fsanitize=address,undefined -fno-sanitize-recover=all \
//         -I src/navi48-bringup/src/apple src/navi48-bringup/tests/gfx_ramtop_test.cpp -o /tmp/rttest && /tmp/rttest
#include <cstdio>
#include <cstdint>
#include <cstring>
#include "gfx_ramtop.h"

static int gFail = 0, gRun = 0, gQuiet = 0;

static void ck(const char *what, uint64_t got, uint64_t want)
{
    gRun++;
    if (got != want) {
        gFail++;
        if (!gQuiet) std::printf("  FAIL %-70s got %#llx want %#llx\n", what,
                                 (unsigned long long)got, (unsigned long long)want);
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// The functions under test, reachable through pointers so a mutant can be swapped in.
// ---------------------------------------------------------------------------------------------------------------------
typedef uint32_t (*RefuseFn)(uint64_t, uint64_t);
typedef uint32_t (*EvalFn)(const n48_rt_map *, n48_rt_res *);

static RefuseFn gRefuse = &n48_rt_refuse;
static EvalFn   gEval   = &n48_rt_eval;

// ---------------------------------------------------------------------------------------------------------------------
// SYNTHETIC EFI MEMORY MAPS
// ---------------------------------------------------------------------------------------------------------------------
// The descriptor stride is MemoryMapDescriptorSize, not sizeof(EfiMemoryRange) (40): real firmware pads to 48. The pad
// bytes are filled with 0xCC and VirtualStart/Attribute with junk, so a parser that strides by the struct or trusts a
// field it should ignore reads nonsense and is caught.
struct Desc { uint32_t type; uint64_t start; uint64_t pages; };

#define KB (1024ull)
#define MB (1024ull * 1024ull)
#define GB (1024ull * 1024ull * 1024ull)
#define PAGES(bytes) ((bytes) / 4096ull)

static uint8_t gBuf[(size_t)N48_RT_DESC_MAX * N48_RT_DESC_COUNT_MAX];

static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static void put64(uint8_t *p, uint64_t v) { put32(p, (uint32_t)v); put32(p + 4, (uint32_t)(v >> 32)); }

// Returns MemoryMapSize.
static uint64_t build(const Desc *d, uint32_t n, uint32_t dsz)
{
    for (size_t i = 0; i < (size_t)dsz * n; i++) gBuf[i] = 0xCC;
    for (uint32_t i = 0; i < n; i++) {
        uint8_t *p = gBuf + (size_t)i * dsz;
        put32(p + 0, d[i].type);
        put32(p + 4, 0xDEADBEEFu);               /* Pad — must be ignored */
        put64(p + 8, d[i].start);
        put64(p + 16, 0xFFFFFF8000000000ull + i); /* VirtualStart — must be ignored */
        put64(p + 24, d[i].pages);
        put64(p + 32, 0x800000000000000Full);     /* Attribute — must be ignored */
    }
    return (uint64_t)dsz * n;
}

// THE MACHINE THIS PROJECT RUNS ON: 32 GiB of DDR4 (hw.memsize 34359738368 = 0x800000000 exactly), TOLUD at
// 0xD0000000 (arm11 read BAR0 there), so 0x730000000 of DRAM is remapped above 4 GiB and the top of RAM is
// 0x830000000. That is the whole defect in one line: the guard's constant IS hw.memsize, and real RAM is above it.
static const Desc kMap32[] = {
    { N48_RT_EFI_CONVENTIONAL, 0x00000000ull, PAGES(640 * KB) },
    { N48_RT_EFI_RESERVED,     0x000A0000ull, PAGES(384 * KB) },          /* legacy hole - not RAM */
    { N48_RT_EFI_CONVENTIONAL, 0x00100000ull, PAGES(0xBFF00000ull) },     /* .. 0xC0000000 */
    { N48_RT_EFI_BS_DATA,      0xC0000000ull, PAGES(128 * MB) },
    { N48_RT_EFI_RS_DATA,      0xC8000000ull, PAGES(16 * MB) },
    { N48_RT_EFI_ACPI_NVS,     0xC9000000ull, PAGES(16 * MB) },
    { N48_RT_EFI_RESERVED,     0xCA000000ull, PAGES(96 * MB) },           /* TSEG etc - DRAM, withdrawn, not counted */
    { N48_RT_EFI_MMIO,         0xD0000000ull, PAGES(768 * MB) },          /* the 32-bit PCI hole, ends exactly at 4 GiB */
    { N48_RT_EFI_CONVENTIONAL, 0x100000000ull, PAGES(0x730000000ull) },   /* .. 0x830000000 - the remapped bank */
};
#define MAP32_N ((uint32_t)(sizeof(kMap32) / sizeof(kMap32[0])))
static const uint64_t kPhys32 = 32ull * GB;
static const uint64_t kTop32  = 0x830000000ull;

// THE REAL MACHINE AS MEASURED: installed DRAM 32768 MiB (= 0x800000000) and a derived top of 0x83F380000. It is the
// kMap32 shape (the remapped conventional bank ends at 0x830000000) plus the small firmware region the real map carries
// 255.4 MiB higher. The task names only the derived top and the installed figure; this per-descriptor split is the
// reconstruction that reproduces them. The new REMAP CEILING (phys_mem + 4 GiB = 0x900000000) must not touch it.
static const Desc kMapReal[] = {
    { N48_RT_EFI_CONVENTIONAL, 0x00000000ull, PAGES(640 * KB) },
    { N48_RT_EFI_RESERVED,     0x000A0000ull, PAGES(384 * KB) },          /* legacy hole - not RAM */
    { N48_RT_EFI_CONVENTIONAL, 0x00100000ull, PAGES(0xBFF00000ull) },     /* .. 0xC0000000 */
    { N48_RT_EFI_BS_DATA,      0xC0000000ull, PAGES(128 * MB) },
    { N48_RT_EFI_RS_DATA,      0xC8000000ull, PAGES(16 * MB) },
    { N48_RT_EFI_ACPI_NVS,     0xC9000000ull, PAGES(16 * MB) },
    { N48_RT_EFI_RESERVED,     0xCA000000ull, PAGES(96 * MB) },           /* TSEG etc - DRAM, withdrawn, not counted */
    { N48_RT_EFI_MMIO,         0xD0000000ull, PAGES(768 * MB) },          /* the 32-bit PCI hole, ends exactly at 4 GiB */
    { N48_RT_EFI_CONVENTIONAL, 0x100000000ull, PAGES(0x730000000ull) },   /* .. 0x830000000 - the remapped bank */
    { N48_RT_EFI_ACPI_NVS,     0x83F000000ull, PAGES(0x380000ull) },      /* .. 0x83F380000 - the measured real top */
};
#define MAPREAL_N ((uint32_t)(sizeof(kMapReal) / sizeof(kMapReal[0])))
static const uint64_t kTopReal = 0x83F380000ull;

// arm12's two refused host pages, 5.35 MiB and 156.3 MiB above the old constant.
static const uint64_t kArm12A = 0x800559000ull;
static const uint64_t kArm12B = 0x809c47000ull;

static void map_in(n48_rt_map &m, uint64_t bytes, uint32_t dsz, uint64_t phys)
{
    m.desc = gBuf; m.bytes = bytes; m.desc_size = dsz; m.phys_mem = phys; m.floor = N48_RT_LEGACY_TOP;
}

// ---------------------------------------------------------------------------------------------------------------------
// THE MUTANT EVALUATOR — one copy of n48_rt_eval with a defect selector. Defect 0 is the faithful copy and the test
// asserts it agrees with the real header on every fixture, so a drift between the two is itself caught.
// ---------------------------------------------------------------------------------------------------------------------
enum {
    MD_NONE = 0, MD_MMIO_IS_RAM, MD_NO_DEVICE, MD_NO_FLOOR, MD_VALUE_ON_FAIL, MD_NO_TOTAL,
    MD_STRIDE_40, MD_NO_ALIGN, MD_NO_TYPE_BAND, MD_APERTURE_FIRST_ONLY, MD_TOP_OVER_ALL
};

static int md_is_ram(uint32_t t, int d) { if (d == MD_MMIO_IS_RAM && n48_rt_is_device(t)) return 1; return n48_rt_is_ram(t); }
static int md_is_dev(uint32_t t, int d) { if (d == MD_NO_DEVICE) return 0; return n48_rt_is_device(t); }

static uint32_t mut_eval(const n48_rt_map *m, n48_rt_res *r, int d)
{
    uint64_t prev_end = 0u, count = 0u, i;
    const uint64_t floor_v = (m && m->floor) ? m->floor : N48_RT_LEGACY_TOP;
    const uint64_t stride  = (d == MD_STRIDE_40) ? 40ull : (m ? (uint64_t)m->desc_size : 0ull);

    if (!r) return N48_RT_NO_MAP;
    { n48_rt_res z; uint32_t k;
      z.reason = N48_RT_NO_MAP; z.value = floor_v; z.derived = 0u; z.floor = floor_v;
      z.descs = 0u; z.ram_descs = 0u; z.dev_descs = 0u; z.empty_descs = 0u; z.unsorted = 0u;
      z.ram_bytes = 0u; z.tops = 0u; z.bad_index = 0u; z.bad_type = 0u; z.bad_start = 0u; z.bad_end = 0u;
      for (k = 0u; k < N48_RT_TOPS; k++) { z.top_start[k] = 0u; z.top_end[k] = 0u; z.top_type[k] = 0u; }
      *r = z; }
    if (!m || !m->desc) return r->reason = N48_RT_NO_MAP, r->reason;
    if (m->desc_size < N48_RT_DESC_MIN || m->desc_size > N48_RT_DESC_MAX || (m->desc_size % 8u) != 0u)
        return r->reason = N48_RT_BAD_HEADER, r->reason;
    if (m->bytes == 0u || (m->bytes % (uint64_t)m->desc_size) != 0u) return r->reason = N48_RT_BAD_HEADER, r->reason;
    count = m->bytes / (uint64_t)m->desc_size;
    if (count == 0u || count > (uint64_t)N48_RT_DESC_COUNT_MAX) return r->reason = N48_RT_BAD_HEADER, r->reason;

    for (i = 0u; i < count; i++) {
        const uint8_t *p = m->desc + i * stride;
        const uint32_t type  = n48_rt_ld32(p + N48_RT_OFF_TYPE);
        const uint64_t start = n48_rt_ld64(p + N48_RT_OFF_START);
        const uint64_t pages = n48_rt_ld64(p + N48_RT_OFF_PAGES);
        uint64_t end;
        r->descs++;
        r->bad_index = (uint32_t)i; r->bad_type = type; r->bad_start = start; r->bad_end = 0u;
        if (d != MD_NO_TYPE_BAND && !n48_rt_type_ok(type)) goto bad;
        if (d != MD_NO_ALIGN && (start & (N48_RT_PAGE - 1u)) != 0u) goto bad;
        if (start >= N48_RT_ADDR_MAX) goto bad;
        if (pages == 0u) { r->empty_descs++; continue; }
        if (pages > N48_RT_ADDR_MAX / N48_RT_PAGE) goto bad;
        end = start + pages * N48_RT_PAGE;
        r->bad_end = end;
        if (end <= start || end > N48_RT_ADDR_MAX) goto bad;
        if (start < prev_end) r->unsorted++;
        if (end > prev_end) prev_end = end;
        if (d == MD_TOP_OVER_ALL && end > r->derived) r->derived = end;
        if (md_is_ram(type, d)) {
            r->ram_descs++; r->ram_bytes += pages * N48_RT_PAGE;
            if (end > r->derived) r->derived = end;
            n48_rt_top_push(r, start, end, type);
        } else if (md_is_dev(type, d)) {
            r->dev_descs++;
        }
        continue;
bad:
        r->reason = N48_RT_BAD_DESC;
        if (d == MD_VALUE_ON_FAIL && r->derived) r->value = r->derived;
        return r->reason;
    }
    r->bad_index = 0u; r->bad_type = 0u; r->bad_start = 0u; r->bad_end = 0u;
    if (r->ram_descs == 0u || r->derived == 0u) return r->reason = N48_RT_NO_RAM, r->reason;
    if (d != MD_NO_TOTAL) {
        if (m->phys_mem) {
            if (r->ram_bytes < m->phys_mem - m->phys_mem / 8u) return r->reason = N48_RT_TOTAL_MISMATCH, r->reason;
            if (r->ram_bytes > m->phys_mem + m->phys_mem / 64u) return r->reason = N48_RT_TOTAL_MISMATCH, r->reason;
        } else if (r->ram_descs < 4u || r->ram_bytes < (1ull << 30)) {
            return r->reason = N48_RT_TOTAL_MISMATCH, r->reason;
        }
    }
    // The REMAP CEILING is part of the real evaluator, so the faithful copy carries it unconditionally; it has no mutant
    // selector of its own (the authoritative non-vacuity proof for it is breaking the REAL header, per the file header).
    if (m->phys_mem && r->derived > m->phys_mem + N48_RT_REMAP_MAX)
        return r->reason = N48_RT_TOP_ABOVE_PHYS, r->reason;
    for (i = 0u; i < count; i++) {
        const uint8_t *p = m->desc + i * stride;
        const uint32_t type  = n48_rt_ld32(p + N48_RT_OFF_TYPE);
        const uint64_t start = n48_rt_ld64(p + N48_RT_OFF_START);
        const uint64_t pages = n48_rt_ld64(p + N48_RT_OFF_PAGES);
        uint64_t end;
        if (d == MD_APERTURE_FIRST_ONLY && i > 0u) break;
        if (pages == 0u || !md_is_dev(type, d)) continue;
        end = start + pages * N48_RT_PAGE;
        if (start < r->derived && end > N48_RT_HOLE_HI) {
            r->bad_index = (uint32_t)i; r->bad_type = type; r->bad_start = start; r->bad_end = end;
            r->reason = N48_RT_APERTURE;
            if (d == MD_VALUE_ON_FAIL) r->value = r->derived;
            return r->reason;
        }
    }
    r->reason = N48_RT_OK;
    r->value  = (d == MD_NO_FLOOR) ? r->derived : ((r->derived < floor_v) ? floor_v : r->derived);
    return r->reason;
}

#define MUT_EVAL(name, defect) \
    static uint32_t name(const n48_rt_map *m, n48_rt_res *r) { return mut_eval(m, r, defect); }
MUT_EVAL(e_faithful,      MD_NONE)
MUT_EVAL(e_mmio_is_ram,   MD_MMIO_IS_RAM)
MUT_EVAL(e_no_device,     MD_NO_DEVICE)
MUT_EVAL(e_no_floor,      MD_NO_FLOOR)
MUT_EVAL(e_value_on_fail, MD_VALUE_ON_FAIL)
MUT_EVAL(e_no_total,      MD_NO_TOTAL)
MUT_EVAL(e_stride_40,     MD_STRIDE_40)
MUT_EVAL(e_no_align,      MD_NO_ALIGN)
MUT_EVAL(e_no_type_band,  MD_NO_TYPE_BAND)
MUT_EVAL(e_ap_first,      MD_APERTURE_FIRST_ONLY)
MUT_EVAL(e_top_over_all,  MD_TOP_OVER_ALL)

// n48_rt_refuse mutants — swapped in by pointer, so these ARE mutations of the real predicate's use.
static uint32_t r_strict(uint64_t page, uint64_t top)
{ if (page > top) return N48_RT_REFUSE_TOP; if (page >= N48_RT_HOLE_LO && page < N48_RT_HOLE_HI) return N48_RT_REFUSE_HOLE; return N48_RT_ACCEPT; }
static uint32_t r_no_hole(uint64_t page, uint64_t top)
{ if (page >= top) return N48_RT_REFUSE_TOP; return N48_RT_ACCEPT; }
static uint32_t r_no_top(uint64_t page, uint64_t top)
{ (void)top; if (page >= N48_RT_HOLE_LO && page < N48_RT_HOLE_HI) return N48_RT_REFUSE_HOLE; return N48_RT_ACCEPT; }

// ---------------------------------------------------------------------------------------------------------------------
// THE CHECKS
// ---------------------------------------------------------------------------------------------------------------------
static void all_checks()
{
    n48_rt_map m {};
    n48_rt_res r {};

    // ---- 5. THE PCI-HOLE HALF IS'S, UNCHANGED, FOR EVERY BOUND ----
    {
        const uint64_t tops[] = { N48_RT_LEGACY_TOP, kTop32, 0x1000000000ull };
        for (uint64_t t : tops) {
            ck("hole lo edge refused",      gRefuse(0xA0000000ull, t), N48_RT_REFUSE_HOLE);
            ck("hole interior refused",     gRefuse(0xC0000000ull, t), N48_RT_REFUSE_HOLE);
            ck("hole hi edge - 1 refused",  gRefuse(0xFFFFF000ull, t), N48_RT_REFUSE_HOLE);
            ck("just below the hole ok",    gRefuse(0x9FFFF000ull, t), N48_RT_ACCEPT);
            ck("4 GiB exactly ok",          gRefuse(0x100000000ull, t), N48_RT_ACCEPT);
            ck("page 0 ok",                 gRefuse(0x0ull, t), N48_RT_ACCEPT);
            ck("at the top refused",        gRefuse(t, t), N48_RT_REFUSE_TOP);
            ck("one page below the top ok", gRefuse(t - 4096ull, t), N48_RT_ACCEPT);
            ck("far above the top refused", gRefuse(t + (64ull * GB), t), N48_RT_REFUSE_TOP);
        }
    }

    // ---- 1. THE 32 GiB MACHINE WITH THE HOLE, at three descriptor strides ----
    {
        const uint32_t strides[] = { 40u, 48u, 56u };
        for (uint32_t dsz : strides) {
            const uint64_t bytes = build(kMap32, MAP32_N, dsz);
            map_in(m, bytes, dsz, kPhys32);
            ck("32 GiB map: reason OK",        gEval(&m, &r), N48_RT_OK);
            ck("32 GiB map: derived",          r.derived, kTop32);
            ck("32 GiB map: value",            r.value, kTop32);
            ck("32 GiB map: value >= 0x830000000", r.value >= 0x830000000ull ? 1u : 0u, 1u);
            ck("32 GiB map: descriptors",      r.descs, MAP32_N);
            ck("32 GiB map: RAM descriptors",  r.ram_descs, 6u);
            ck("32 GiB map: device windows",   r.dev_descs, 1u);
            ck("32 GiB map: none out of order", r.unsorted, 0u);
            ck("32 GiB map: top RAM start",    r.top_start[0], 0x100000000ull);
            ck("32 GiB map: top RAM end",      r.top_end[0], kTop32);
            ck("32 GiB map: 2nd RAM end",      r.top_end[1], 0xCA000000ull);   /* ACPI NVS, the highest sub-4 GiB RAM */
            ck("32 GiB map: 3rd RAM end",      r.top_end[2], 0xC9000000ull);   /* runtime-services data below it */
            ck("32 GiB map: tops kept",        r.tops, 3u);
            // THE DEFECT ARM12 REPRODUCED: both refused host pages are now accepted.
            ck("arm12 page 0x800559000 accepted", gRefuse(kArm12A, r.value), N48_RT_ACCEPT);
            ck("arm12 page 0x809c47000 accepted", gRefuse(kArm12B, r.value), N48_RT_ACCEPT);
            ck("the top itself is still refused", gRefuse(kTop32, r.value), N48_RT_REFUSE_TOP);
            ck("a page above the top refused",    gRefuse(kTop32 + 4096ull, r.value), N48_RT_REFUSE_TOP);
            ck("the PCI hole is still refused",   gRefuse(0xD0000000ull, r.value), N48_RT_REFUSE_HOLE);
        }
    }

    // ---- 2a. A 64-BIT PCI APERTURE ABOVE RAM IS EXCLUDED ----
    {
        Desc d[MAP32_N + 1];
        for (uint32_t i = 0; i < MAP32_N; i++) d[i] = kMap32[i];
        d[MAP32_N] = Desc { N48_RT_EFI_MMIO, 0x1000000000ull, PAGES(256 * MB) };   /* 64 GiB, above TOM2 */
        const uint64_t bytes = build(d, MAP32_N + 1u, 48u);
        map_in(m, bytes, 48u, kPhys32);
        ck("aperture above RAM: reason OK",      gEval(&m, &r), N48_RT_OK);
        ck("aperture above RAM: derived is RAM", r.derived, kTop32);
        ck("aperture above RAM: value is RAM",   r.value, kTop32);
        ck("aperture above RAM: device windows", r.dev_descs, 2u);
        ck("A PAGE INSIDE THE APERTURE IS REFUSED", gRefuse(0x1000000000ull, r.value), N48_RT_REFUSE_TOP);
        ck("the aperture's last page is refused",   gRefuse(0x100FFF000ull + 0xF00000000ull, r.value), N48_RT_REFUSE_TOP);
    }

    // ---- 2b. THE PLANTED DEFECT THAT MATTERS: a device window INSIDE the band we would otherwise accept ----
    // RAM tops at 0x830000000 and the firmware put a device window at [0x810000000, 0x820000000) - above the legacy
    // constant, below the derived top. Widening to 0x830000000 would map it. The derivation must be REFUSED outright.
    {
        Desc d[MAP32_N + 1];
        for (uint32_t i = 0; i < MAP32_N; i++) d[i] = kMap32[i];
        d[MAP32_N] = Desc { N48_RT_EFI_MMIO, 0x810000000ull, PAGES(256 * MB) };
        const uint64_t bytes = build(d, MAP32_N + 1u, 48u);
        map_in(m, bytes, 48u, kPhys32);
        ck("window inside the band: REFUSED",    gEval(&m, &r), N48_RT_APERTURE);
        ck("window inside the band: value is the legacy constant", r.value, N48_RT_LEGACY_TOP);
        ck("window inside the band: named",      r.bad_start, 0x810000000ull);
        ck("window inside the band: its type",   r.bad_type, N48_RT_EFI_MMIO);
        ck("A PAGE INSIDE THAT WINDOW IS REFUSED", gRefuse(0x810000000ull, r.value), N48_RT_REFUSE_TOP);
        ck("arm12's pages go back to refused too", gRefuse(kArm12A, r.value), N48_RT_REFUSE_TOP);
    }

    // ---- 3. EVERY UNREADABLE / IMPLAUSIBLE MAP LANDS ON THE LEGACY CONSTANT ----
    {
        const uint64_t bytes = build(kMap32, MAP32_N, 48u);

        map_in(m, bytes, 48u, kPhys32); m.desc = nullptr;
        ck("null map: reason", gEval(&m, &r), N48_RT_NO_MAP);
        ck("null map: value",  r.value, N48_RT_LEGACY_TOP);

        map_in(m, 0u, 48u, kPhys32);
        ck("zero-length map: reason", gEval(&m, &r), N48_RT_BAD_HEADER);
        ck("zero-length map: value",  r.value, N48_RT_LEGACY_TOP);

        map_in(m, bytes, 7u, kPhys32);
        ck("descriptor size 7: reason", gEval(&m, &r), N48_RT_BAD_HEADER);
        ck("descriptor size 7: value",  r.value, N48_RT_LEGACY_TOP);

        map_in(m, 39u * MAP32_N, 39u, kPhys32);
        ck("descriptor size below the struct: reason", gEval(&m, &r), N48_RT_BAD_HEADER);

        map_in(m, 264ull * 4ull, 264u, kPhys32);
        ck("descriptor size above the cap: reason", gEval(&m, &r), N48_RT_BAD_HEADER);

        map_in(m, bytes + 1ull, 48u, kPhys32);
        ck("map size not a multiple of the stride: reason", gEval(&m, &r), N48_RT_BAD_HEADER);

        map_in(m, 40ull * 1025ull, 40u, kPhys32);
        ck("1025 descriptors: reason", gEval(&m, &r), N48_RT_BAD_HEADER);
        ck("1025 descriptors: value",  r.value, N48_RT_LEGACY_TOP);

        // RECYCLED BYTES. boot_args.MemoryMap names physical pages the kernel may have reclaimed; the defence is
        // validation, so a deterministic pile of pseudo-random bytes must never come back OK.
        uint32_t x = 0x12345678u;
        int okSeen = 0;
        for (int trial = 0; trial < 64; trial++) {
            for (size_t i = 0; i < 48u * 64u; i++) { x = x * 1103515245u + 12345u; gBuf[i] = (uint8_t)(x >> 16); }
            map_in(m, 48ull * 64ull, 48u, kPhys32);
            if (gEval(&m, &r) == N48_RT_OK) okSeen++;
            ck("recycled bytes: value is the legacy constant unless OK",
               (r.reason == N48_RT_OK || r.value == N48_RT_LEGACY_TOP) ? 1u : 0u, 1u);
        }
        ck("recycled bytes: never derived a bound", (uint64_t)okSeen, 0u);
    }

    // ---- 4. MONOTONE: a smaller machine's derived top is floored at the legacy constant ----
    {
        const Desc small[] = {
            { N48_RT_EFI_CONVENTIONAL, 0x00000000ull, PAGES(640 * KB) },
            { N48_RT_EFI_CONVENTIONAL, 0x00100000ull, PAGES(0xBFF00000ull) },
            { N48_RT_EFI_BS_DATA,      0xC0000000ull, PAGES(128 * MB) },
            { N48_RT_EFI_MMIO,         0xD0000000ull, PAGES(768 * MB) },
            { N48_RT_EFI_CONVENTIONAL, 0x100000000ull, PAGES(0x38000000ull) },   /* .. 0x138000000, sums to just under 4 GiB */
        };
        const uint64_t bytes = build(small, 5u, 48u);
        map_in(m, bytes, 48u, 4ull * GB);
        ck("4 GiB machine: reason OK", gEval(&m, &r), N48_RT_OK);
        ck("4 GiB machine: derived",   r.derived, 0x138000000ull);
        ck("4 GiB machine: value is FLOORED at the legacy constant", r.value, N48_RT_LEGACY_TOP);
        ck("4 GiB machine: a page 0.0.385 accepted is still accepted", gRefuse(0x400000000ull, r.value), N48_RT_ACCEPT);
    }

    // ---- 6. THE REMAP CEILING: the sum cross-check alone cannot bound `derived` ----
    // One small RAM-type descriptor planted at a very high address moves the MAX END arbitrarily while the SUM stays
    // inside the installed-DRAM tolerance. The review found that hole; the derivation must now refuse it.
    {
        // (a) THE REAL MACHINE IS STILL ACCEPTED. Derived 0x83F380000, installed 32768 MiB; the ceiling is
        //     0x800000000 + 4 GiB = 0x900000000, so the real top sits 0x0C0C8000 below it.
        const uint64_t bytes = build(kMapReal, MAPREAL_N, 48u);
        map_in(m, bytes, 48u, kPhys32);
        ck("real machine: reason OK", gEval(&m, &r), N48_RT_OK);
        ck("real machine: derived stays 0x83F380000", r.derived, kTopReal);
        ck("real machine: value stays 0x83F380000", r.value, kTopReal);
        ck("real machine: under the remap ceiling", r.derived <= kPhys32 + 4ull * GB ? 1u : 0u, 1u);

        // (b) THE PLANTED DEFECT: one 2 MiB Conventional-RAM descriptor at 128 GiB. The sum moves by 2 MiB on 32 GiB
        //     (nothing the total cross-check can see), the max end jumps to 0x2000200000, and before this check the
        //     derivation returned that as the bound. REFUSED now, and the value is the legacy floor.
        Desc d[MAPREAL_N + 1];
        for (uint32_t i = 0; i < MAPREAL_N; i++) d[i] = kMapReal[i];
        d[MAPREAL_N] = Desc { N48_RT_EFI_CONVENTIONAL, 0x2000000000ull, PAGES(2 * MB) };
        uint64_t bytes2 = build(d, MAPREAL_N + 1u, 48u);
        map_in(m, bytes2, 48u, kPhys32);
        ck("128 GiB RAM spike: REFUSED", gEval(&m, &r), N48_RT_TOP_ABOVE_PHYS);
        ck("128 GiB RAM spike: value is the legacy constant", r.value, N48_RT_LEGACY_TOP);
        ck("128 GiB RAM spike: derived is still reported", r.derived, 0x2000200000ull);
        ck("128 GiB RAM spike: named reason string",
           (uint64_t)std::strcmp(n48_rt_reason_name(r.reason), "DERIVED TOP ABOVE INSTALLED DRAM + 4 GiB REMAP CEILING - FALLBACK"), 0u);
        ck("A PAGE INSIDE THE SPIKE IS REFUSED", gRefuse(0x2000000000ull, r.value), N48_RT_REFUSE_TOP);

        // (c) THE BOUNDARY: derived == phys_mem + 4 GiB exactly is ACCEPTED; one page above is REFUSED.
        Desc b[MAPREAL_N + 1];
        for (uint32_t i = 0; i < MAPREAL_N; i++) b[i] = kMapReal[i];
        b[MAPREAL_N] = Desc { N48_RT_EFI_CONVENTIONAL, 0x8F0000000ull, PAGES(0x10000000ull) };   /* .. 0x900000000 */
        uint64_t bytes3 = build(b, MAPREAL_N + 1u, 48u);
        map_in(m, bytes3, 48u, kPhys32);
        ck("at the ceiling exactly: reason OK", gEval(&m, &r), N48_RT_OK);
        ck("at the ceiling exactly: derived", r.derived, kPhys32 + 4ull * GB);
        ck("at the ceiling exactly: value", r.value, kPhys32 + 4ull * GB);

        for (uint32_t i = 0; i < MAPREAL_N; i++) b[i] = kMapReal[i];
        b[MAPREAL_N] = Desc { N48_RT_EFI_CONVENTIONAL, 0x8FFFFF000ull, 2ull };   /* 2 pages .. 0x900001000, one page over */
        uint64_t bytes4 = build(b, MAPREAL_N + 1u, 48u);
        map_in(m, bytes4, 48u, kPhys32);
        ck("one page over the ceiling: REFUSED", gEval(&m, &r), N48_RT_TOP_ABOVE_PHYS);
        ck("one page over the ceiling: value is the legacy constant", r.value, N48_RT_LEGACY_TOP);
    }

    // ---- BAD DESCRIPTORS ----
    {
        Desc d[MAP32_N];
        for (uint32_t i = 0; i < MAP32_N; i++) d[i] = kMap32[i];
        d[4].start |= 0x800ull;                                   /* not 4 KiB aligned */
        uint64_t bytes = build(d, MAP32_N, 48u);
        map_in(m, bytes, 48u, kPhys32);
        ck("misaligned descriptor: reason", gEval(&m, &r), N48_RT_BAD_DESC);
        ck("misaligned descriptor: value",  r.value, N48_RT_LEGACY_TOP);
        ck("misaligned descriptor: index",  r.bad_index, 4u);

        for (uint32_t i = 0; i < MAP32_N; i++) d[i] = kMap32[i];
        d[3].type = 20u;                                          /* outside the enum and below the OEM band */
        bytes = build(d, MAP32_N, 48u);
        map_in(m, bytes, 48u, kPhys32);
        ck("type out of band: reason", gEval(&m, &r), N48_RT_BAD_DESC);
        ck("type out of band: value",  r.value, N48_RT_LEGACY_TOP);

        for (uint32_t i = 0; i < MAP32_N; i++) d[i] = kMap32[i];
        d[8].pages = 0xFFFFFFFFFFFFFFFFull;                       /* overflow */
        bytes = build(d, MAP32_N, 48u);
        map_in(m, bytes, 48u, kPhys32);
        ck("page count overflow: reason", gEval(&m, &r), N48_RT_BAD_DESC);
        ck("page count overflow: value",  r.value, N48_RT_LEGACY_TOP);

        // An OEM/OS type is tolerated and is NEVER RAM.
        for (uint32_t i = 0; i < MAP32_N; i++) d[i] = kMap32[i];
        d[6].type = 0x80000001u;
        bytes = build(d, MAP32_N, 48u);
        map_in(m, bytes, 48u, kPhys32);
        ck("OEM type tolerated: reason", gEval(&m, &r), N48_RT_OK);
        ck("OEM type tolerated: not RAM", r.ram_descs, 6u);
        ck("OEM type tolerated: not a device window", r.dev_descs, 1u);

        // A zero-page descriptor is skipped, not fatal.
        for (uint32_t i = 0; i < MAP32_N; i++) d[i] = kMap32[i];
        d[1].pages = 0ull;
        bytes = build(d, MAP32_N, 48u);
        map_in(m, bytes, 48u, kPhys32);
        ck("empty descriptor: reason", gEval(&m, &r), N48_RT_OK);
        ck("empty descriptor: counted", r.empty_descs, 1u);
    }

    // ---- THE TOTAL CROSS-CHECK ----
    {
        const Desc half[] = {
            { N48_RT_EFI_CONVENTIONAL, 0x00100000ull, PAGES(0xBFF00000ull) },
            { N48_RT_EFI_MMIO,         0xD0000000ull, PAGES(768 * MB) },
            { N48_RT_EFI_CONVENTIONAL, 0x100000000ull, PAGES(0x40000000ull) },
        };
        uint64_t bytes = build(half, 3u, 48u);
        map_in(m, bytes, 48u, kPhys32);                   /* claims 32 GiB installed, describes ~4 GiB */
        ck("map describes far less than is installed: reason", gEval(&m, &r), N48_RT_TOTAL_MISMATCH);
        ck("map describes far less than is installed: value",  r.value, N48_RT_LEGACY_TOP);

        const Desc over[] = {
            { N48_RT_EFI_CONVENTIONAL, 0x00100000ull, PAGES(0xBFF00000ull) },
            { N48_RT_EFI_CONVENTIONAL, 0x100000000ull, PAGES(0x1000000000ull) },   /* 64 GiB on a 32 GiB machine */
        };
        bytes = build(over, 2u, 48u);
        map_in(m, bytes, 48u, kPhys32);
        ck("map claims more RAM than is installed: reason", gEval(&m, &r), N48_RT_TOTAL_MISMATCH);
        ck("map claims more RAM than is installed: value",  r.value, N48_RT_LEGACY_TOP);

        // With no PhysicalMemorySize the cross-check is a shape test, and it is weaker - say so by testing it.
        bytes = build(kMap32, MAP32_N, 48u);
        map_in(m, bytes, 48u, 0ull);
        ck("no installed-DRAM figure: still derives", gEval(&m, &r), N48_RT_OK);
        ck("no installed-DRAM figure: value",         r.value, kTop32);

        const Desc tiny[] = {
            { N48_RT_EFI_CONVENTIONAL, 0x00100000ull, PAGES(64 * MB) },
            { N48_RT_EFI_MMIO,         0xD0000000ull, PAGES(768 * MB) },
        };
        bytes = build(tiny, 2u, 48u);
        map_in(m, bytes, 48u, 0ull);
        ck("no installed-DRAM figure, implausible map: refused", gEval(&m, &r), N48_RT_TOTAL_MISMATCH);
        ck("no installed-DRAM figure, implausible map: value",   r.value, N48_RT_LEGACY_TOP);
    }

    // ---- OUT-OF-ORDER DESCRIPTORS ARE REPORTED, NEVER FATAL ----
    {
        Desc d[MAP32_N];
        for (uint32_t i = 0; i < MAP32_N; i++) d[i] = kMap32[MAP32_N - 1u - i];
        const uint64_t bytes = build(d, MAP32_N, 48u);
        map_in(m, bytes, 48u, kPhys32);
        ck("reversed map: still derives", gEval(&m, &r), N48_RT_OK);
        ck("reversed map: same top",      r.value, kTop32);
        ck("reversed map: counted as out of order", r.unsorted > 0u ? 1u : 0u, 1u);
    }

    // ---- THE CLASSIFICATION TABLE, stated once and asserted ----
    {
        const uint32_t ram[] = { 1u, 2u, 3u, 4u, 5u, 6u, 7u, 9u, 10u, 13u };
        const uint32_t not_ram[] = { 0u, 8u, 11u, 12u, 14u, 0x70000000u, 0x80000000u };
        for (uint32_t t : ram)     ck("counted as RAM", (uint64_t)n48_rt_is_ram(t), 1u);
        for (uint32_t t : not_ram) ck("not counted as RAM", (uint64_t)n48_rt_is_ram(t), 0u);
        ck("MMIO is a device window",      (uint64_t)n48_rt_is_device(11u), 1u);
        ck("MMIO port is a device window", (uint64_t)n48_rt_is_device(12u), 1u);
        ck("conventional is not a device window", (uint64_t)n48_rt_is_device(7u), 0u);
        ck("type 14 is out of band",  (uint64_t)n48_rt_type_ok(14u), 0u);
        ck("type 13 is in band",      (uint64_t)n48_rt_type_ok(13u), 1u);
        ck("the OEM band is tolerated", (uint64_t)n48_rt_type_ok(0x70000000u), 1u);
    }
}

// ---------------------------------------------------------------------------------------------------------------------
struct Mut { const char *what; EvalFn ef; RefuseFn rf; };

int main()
{
    std::printf("gfx_ramtop_test - the host-page range guard's bound, from the EFI memory map (0.0.386)\n");

    // The mutant evaluator's faithful copy must agree with the real header everywhere, or the mutants prove nothing.
    gEval = &e_faithful; all_checks();
    const int copyFail = gFail;
    gEval = &n48_rt_eval; gFail = 0; gRun = 0;
    std::printf("faithful copy agrees with the real header: %s (%d failures)\n", copyFail ? "NO" : "yes", copyFail);

    all_checks();
    const int realFail = gFail, realRun = gRun;
    std::printf("real rule: %d checks, %d failures\n", realRun, realFail);

    const Mut mut[] = {
        { "D1  a device window is counted as RAM (the brief's defect)", &e_mmio_is_ram,   nullptr },
        { "D2  device windows are never recognised at all",             &e_no_device,     nullptr },
        { "D3  the result is not floored at the legacy constant",       &e_no_floor,      nullptr },
        { "D4  a failure path still returns the derived bound",         &e_value_on_fail, nullptr },
        { "D5  the RAM total is not cross-checked",                     &e_no_total,      nullptr },
        { "D6  the walk strides by sizeof(EfiMemoryRange), not the map's", &e_stride_40,  nullptr },
        { "D7  4 KiB alignment is not checked",                         &e_no_align,      nullptr },
        { "D8  the EfiMemoryType band is not checked",                  &e_no_type_band,  nullptr },
        { "D9  the aperture pass only looks at the first descriptor",   &e_ap_first,      nullptr },
        { "D10 the top is the max over ALL descriptors, not RAM ones",  &e_top_over_all,  nullptr },
        { "D11 the top is tested with > instead of >=",                 nullptr, &r_strict },
        { "D12 the PCI-hole half of the guard is dropped",              nullptr, &r_no_hole },
        { "D13 the RAM-top half of the guard is dropped",               nullptr, &r_no_top },
        { "D14 both halves dropped",                                    nullptr, [](uint64_t, uint64_t) -> uint32_t { return N48_RT_ACCEPT; } },
    };

    int caught = 0;
    for (const Mut &mm : mut) {
        gQuiet = 1; gFail = 0; gRun = 0;
        gEval   = mm.ef ? mm.ef : &n48_rt_eval;
        gRefuse = mm.rf ? mm.rf : &n48_rt_refuse;
        all_checks();
        const int f = gFail, r = gRun;
        gEval = &n48_rt_eval; gRefuse = &n48_rt_refuse; gQuiet = 0;
        std::printf("mutant %-62s %s (%d of %d checks fail)\n", mm.what, f ? "CAUGHT" : "NOT CAUGHT", f, r);
        if (f) caught++;
    }
    const int nmut = (int)(sizeof(mut) / sizeof(mut[0]));
    std::printf("mutants caught %d/%d\n", caught, nmut);
    const int pass = (realFail == 0 && copyFail == 0 && caught == nmut);
    std::printf("%s\n", pass ? "gfx_ramtop: PASS" : "gfx_ramtop: FAIL");
    return pass ? 0 : 1;
}
