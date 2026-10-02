// gfx_ramtop.h — WHERE DOES REAL RAM END ON THIS MACHINE? THE HOST-PAGE RANGE GUARD'S BOUND, FROM THE EFI MEMORY MAP.
// Pure C, host-tested by tests/gfx_ramtop_test.cpp (with planted defects); the kext compiles the SAME header.
// 0.0.386. NOTHING HERE READS OR WRITES HARDWARE, AND THERE IS NO WRITE PRIMITIVE OF ANY KIND: it is arithmetic over
// a byte buffer the caller hands it.
//
// ---------------------------------------------------------------------------------------------------------------------
// THE DEFECT THIS EXISTS TO FIX, AND ITS REPRODUCING BOOT
// ---------------------------------------------------------------------------------------------------------------------
// 0.0.277 gave gfxc_read and its gfxc_write_sys mirror a guard, so that a stale leaf PTE could not turn
// into an IOMemoryDescriptor map of DEVICE MMIO:
//
//     if (page >= 0x800000000ull || (page >= 0xA0000000ull && page < 0x100000000ull)) break;
//
// The left half reads "this machine has 32 GiB, so nothing at or above 32 GiB can be RAM". THAT IS FALSE, and it is
// false BECAUSE the machine has exactly 32 GiB: the sub-4 GiB PCI hole displaces 0.75-1.5 GiB of DRAM, and the memory
// controller remaps it ABOVE the 32 GiB line. arm12 printed the whole 12-line budget of the
// `gfxc-hostreject:` instrument and EVERY line was this half of the guard, `IOMemoryDescriptor map` 0 in all 12, at
// host pages 0x800559000 and 0x809c47000 - 5.35 MiB and 156.3 MiB above the bound, both legitimate RAM. On one boot
// in seven WindowServer's submission pool lands there and the compositor's IBs short-read.
//
// ---------------------------------------------------------------------------------------------------------------------
// WHY THE MAP AND NOT ARITHMETIC - THIS IS THE WHOLE SAFETY ARGUMENT
// ---------------------------------------------------------------------------------------------------------------------
// The tempting fix is `gRamTop = memsize + (4 GiB - TOLUD)`. It is REFUSED here. TOLUD is not known to us (the
// comment guessed BAR0 at 0xC0000000; arm11 read 0xd0000000), and if the real TOLUD is higher than assumed the
// arithmetic produces a bound ABOVE true TOM2. On AMD boards the band immediately above TOM2 is exactly where firmware
// places the 64-BIT PCI APERTURE - the device windows this guard exists to keep out. An over-estimate therefore does
// not merely fail to help, it DELETES THE GUARD'S PURPOSE in the one band that matters.
//
// So the bound is read from the firmware's own description of physical memory - the EFI memory map boot.efi handed the
// kernel - and a descriptor is counted as RAM only if its EfiMemoryType is one the firmware says is backed by DRAM.
// kEfiMemoryMappedIO and kEfiMemoryMappedIOPortSpace are NEVER counted, and a map in which any such descriptor
// overlaps the band we are about to accept is REFUSED OUTRIGHT (n48_rt_eval's aperture pass below): a map we do not
// understand must not widen a safety guard.
//
// THE TYPES COUNTED AS RAM, and why - this mirrors what xnu's own i386_vm_init.c folds into `sane_size`:
//     kEfiLoaderCode (1), kEfiLoaderData (2), kEfiBootServicesCode (3), kEfiBootServicesData (4),
//     kEfiConventionalMemory (7)   - DRAM the OS owns or will own; every DMA buffer a driver is handed comes from here.
//     kEfiRuntimeServicesCode (5), kEfiRuntimeServicesData (6), kEfiACPIReclaimMemory (9), kEfiACPIMemoryNVS (10),
//     kEfiPalCode (13)             - DRAM the firmware keeps. Counted because the question this answers is "can a
//                                    physical page at this address be DRAM at all", not "may the OS allocate it".
// NOT counted: kEfiReservedMemoryType (0) and kEfiUnusableMemory (8) - DRAM the firmware has withdrawn (TSEG and the
// like), excluded because a page there is never handed to a driver, and excluding it can only TIGHTEN the bound;
// kEfiMemoryMappedIO (11) and kEfiMemoryMappedIOPortSpace (12) - device windows, the thing the guard excludes.
// A type in the UEFI OEM/OS band (>= 0x70000000) is tolerated and counted as NOT RAM. Any other out-of-range type is
// treated as a corrupt map.
//
// ---------------------------------------------------------------------------------------------------------------------
// THE CHANGE IS MONOTONE: IT CAN ONLY ACCEPT MORE, NEVER FEWER
// ---------------------------------------------------------------------------------------------------------------------
// The result is floored at the legacy constant (n48_rt_eval's `floor`). A derived top BELOW 0x800000000 would make the
// guard tighter than the one every armed boot to date has run, i.e. would start refusing pages that are accepted
// today - a regression on a working path dressed up as a fix. So the value used is max(derived, floor), and on every
// failure path it is exactly `floor`. 0.0.385's behaviour is the floor of this change, not a branch of it.
//
// ---------------------------------------------------------------------------------------------------------------------
// WHAT THIS HEADER CANNOT DO
// ---------------------------------------------------------------------------------------------------------------------
// It cannot tell a valid map from a plausible-looking pile of recycled bytes with certainty. boot_args.MemoryMap is a
// PHYSICAL address of pages the kernel may have reclaimed (kEfiBootServicesData is added to the free list). The
// defence is entirely in validation: descriptor-size and count bounds, a type band, 4 KiB alignment, overflow bounds,
// and - the strong one - the RAM total must agree with boot_args.PhysicalMemorySize. A pile of recycled bytes does not
// sum to within an eighth of the installed DRAM across exactly the counted types. When any check fails the caller gets
// `floor` and a reason, and the reason is printed.
#ifndef N48_GFX_RAMTOP_H
#define N48_GFX_RAMTOP_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------------------------------------------------
// (a) THE GUARD PREDICATE ITSELF - ONE DEFINITION, BOTH SITES
// ---------------------------------------------------------------------------------------------------------------------
// gfxc_read and gfxc_write_sys used to carry byte-identical copies of the expression. They now call this, so "the two
// sites are identical" is true by construction rather than by inspection. THE PCI-HOLE HALF IS'S, UNCHANGED:
// [0xA0000000, 0x100000000) is where BAR0 and the 32-bit MMIO windows live and nothing in this change touches it.
#define N48_RT_LEGACY_TOP 0x800000000ull   /* 0.0.277: the constant this replaces, and the floor it becomes */
#define N48_RT_HOLE_LO    0xA0000000ull    /* verbatim */
#define N48_RT_HOLE_HI    0x100000000ull   /* verbatim */
#define N48_RT_PAGE       4096ull
// The largest PCI hole this platform family can have below 4 GiB and have DISPLACED ABOVE the DRAM top. The sub-4 GiB
// hole (BAR0 and the 32-bit windows) is the only reason any DRAM sits above the installed total, so 4 GiB is the most
// the derived top may exceed PhysicalMemorySize by; anything further is a descriptor that is not this machine's RAM.
#define N48_RT_REMAP_MAX  (4ull * 1024ull * 1024ull * 1024ull)

enum {
    N48_RT_ACCEPT = 0u,       /* the page may be mapped and read/written */
    N48_RT_REFUSE_TOP = 1u,   /* at or above the RAM top - cannot be DRAM on this machine */
    N48_RT_REFUSE_HOLE = 2u   /* inside the 32-bit PCI hole - BAR0 and the MMIO windows */
};

static inline uint32_t n48_rt_refuse(uint64_t page, uint64_t ram_top)
{
    if (page >= ram_top) return N48_RT_REFUSE_TOP;
    if (page >= N48_RT_HOLE_LO && page < N48_RT_HOLE_HI) return N48_RT_REFUSE_HOLE;
    return N48_RT_ACCEPT;
}

static inline const char *n48_rt_refuse_name(uint32_t r)
{
    switch (r) {
    case N48_RT_ACCEPT:      return "accepted";
    case N48_RT_REFUSE_TOP:  return "at or above the RAM top derived from the EFI memory map";
    case N48_RT_REFUSE_HOLE: return "inside the 32-bit PCI hole [0xA0000000, 0x100000000)";
    default:                 return "unknown";
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// (b) EFI MEMORY TYPES - the names are pexpert/i386/boot.h's enum, quoted
// ---------------------------------------------------------------------------------------------------------------------
#define N48_RT_EFI_RESERVED        0u
#define N48_RT_EFI_LOADER_CODE     1u
#define N48_RT_EFI_LOADER_DATA     2u
#define N48_RT_EFI_BS_CODE         3u
#define N48_RT_EFI_BS_DATA         4u
#define N48_RT_EFI_RS_CODE         5u
#define N48_RT_EFI_RS_DATA         6u
#define N48_RT_EFI_CONVENTIONAL    7u
#define N48_RT_EFI_UNUSABLE        8u
#define N48_RT_EFI_ACPI_RECLAIM    9u
#define N48_RT_EFI_ACPI_NVS       10u
#define N48_RT_EFI_MMIO           11u
#define N48_RT_EFI_MMIO_PORT      12u
#define N48_RT_EFI_PAL_CODE       13u
#define N48_RT_EFI_MAX            14u          /* kEfiMaxMemoryType */
#define N48_RT_EFI_OEM_LO 0x70000000u          /* UEFI OEM/OS-reserved type band; tolerated, never RAM */

// DRAM-backed, per the table in this file's header comment.
static inline int n48_rt_is_ram(uint32_t type)
{
    switch (type) {
    case N48_RT_EFI_LOADER_CODE: case N48_RT_EFI_LOADER_DATA:
    case N48_RT_EFI_BS_CODE:     case N48_RT_EFI_BS_DATA:
    case N48_RT_EFI_CONVENTIONAL:
    case N48_RT_EFI_RS_CODE:     case N48_RT_EFI_RS_DATA:
    case N48_RT_EFI_ACPI_RECLAIM: case N48_RT_EFI_ACPI_NVS:
    case N48_RT_EFI_PAL_CODE:
        return 1;
    default:
        return 0;
    }
}

// A device window. THIS IS THE 64-BIT PCI APERTURE'S TYPE and the reason the aperture pass below exists.
static inline int n48_rt_is_device(uint32_t type)
{
    return type == N48_RT_EFI_MMIO || type == N48_RT_EFI_MMIO_PORT;
}

// A type we are willing to see in a map at all. Anything else says the bytes are not a memory map.
static inline int n48_rt_type_ok(uint32_t type)
{
    return type < N48_RT_EFI_MAX || type >= N48_RT_EFI_OEM_LO;
}

// ---------------------------------------------------------------------------------------------------------------------
// (c) THE PARSE
// ---------------------------------------------------------------------------------------------------------------------
// sizeof(EfiMemoryRange) is 40 (Type u32, Pad u32, PhysicalStart u64, VirtualStart u64, NumberOfPages u64,
// Attribute u64). MemoryMapDescriptorSize is usually larger (48 on most firmware) and the caller must stride by it,
// never by 40 - striding by the struct is the classic way to read an EFI map into nonsense.
#define N48_RT_DESC_MIN       40u
#define N48_RT_DESC_MAX      256u
#define N48_RT_DESC_COUNT_MAX 1024u
#define N48_RT_ADDR_MAX (1ull << 46)     /* 64 TiB: no descriptor may claim to end above this */
#define N48_RT_TOPS 3u                   /* how many top RAM descriptors are kept for the boot line */

#define N48_RT_OFF_TYPE   0u
#define N48_RT_OFF_START  8u
#define N48_RT_OFF_PAGES 24u

enum {
    N48_RT_OK = 0u,              /* derived cleanly; `value` is max(derived, floor) */
    N48_RT_NO_MAP = 1u,          /* no buffer - PE_state.bootArgs or the map pointer was null */
    N48_RT_BAD_HEADER = 2u,      /* descriptor size / map size / count out of bounds */
    N48_RT_BAD_DESC = 3u,        /* a descriptor is not a descriptor: type band, alignment or overflow */
    N48_RT_NO_RAM = 4u,          /* parsed, and not one descriptor is RAM */
    N48_RT_TOTAL_MISMATCH = 5u,  /* the RAM total disagrees with the installed DRAM: these bytes are not our map */
    N48_RT_APERTURE = 6u,        /* a device window overlaps the band we were about to accept - REFUSE */
    N48_RT_TOP_ABOVE_PHYS = 7u,  /* the max RAM end exceeds installed DRAM by more than the whole sub-4 GiB remap - REFUSE */
    N48_RT_REASONS = 8u
};

static inline const char *n48_rt_reason_name(uint32_t r)
{
    switch (r) {
    case N48_RT_OK:             return "OK (derived from the EFI memory map)";
    case N48_RT_NO_MAP:         return "NO MAP (boot args or map pointer unavailable) - FALLBACK";
    case N48_RT_BAD_HEADER:     return "BAD HEADER (descriptor size, map size or count out of bounds) - FALLBACK";
    case N48_RT_BAD_DESC:       return "BAD DESCRIPTOR (type band, 4 KiB alignment or range overflow) - FALLBACK";
    case N48_RT_NO_RAM:         return "NO RAM DESCRIPTOR - FALLBACK";
    case N48_RT_TOTAL_MISMATCH: return "TOTAL MISMATCH (RAM sum disagrees with installed DRAM) - FALLBACK";
    case N48_RT_APERTURE:       return "DEVICE WINDOW INSIDE THE DERIVED BAND (64-bit PCI aperture) - FALLBACK";
    case N48_RT_TOP_ABOVE_PHYS: return "DERIVED TOP ABOVE INSTALLED DRAM + 4 GiB REMAP CEILING - FALLBACK";
    default:                    return "unknown";
    }
}

typedef struct n48_rt_map {
    const uint8_t *desc;     /* the first EfiMemoryRange; the caller has already mapped the bytes */
    uint64_t       bytes;    /* boot_args.MemoryMapSize */
    uint32_t       desc_size;/* boot_args.MemoryMapDescriptorSize - the stride, NOT sizeof(EfiMemoryRange) */
    uint64_t       phys_mem; /* boot_args.PhysicalMemorySize; 0 = unknown, which weakens the cross-check */
    uint64_t       floor;    /* the result is never below this; 0 means N48_RT_LEGACY_TOP */
} n48_rt_map;

typedef struct n48_rt_res {
    uint32_t reason;
    uint64_t value;          /* WHAT THE GUARD MUST USE. Always set, on every path. */
    uint64_t derived;        /* max end over RAM descriptors before the floor; 0 when nothing was derived */
    uint64_t floor;
    uint32_t descs;          /* descriptors walked */
    uint32_t ram_descs;
    uint32_t dev_descs;
    uint32_t empty_descs;    /* NumberOfPages == 0: skipped, not an error */
    uint32_t unsorted;       /* descriptors starting below the previous end - reported, never fatal */
    uint64_t ram_bytes;
    uint64_t top_start[N48_RT_TOPS];   /* the highest-ending RAM descriptors, for the boot line */
    uint64_t top_end[N48_RT_TOPS];
    uint32_t top_type[N48_RT_TOPS];
    uint32_t tops;
    uint32_t bad_index;      /* for BAD_DESC and APERTURE: which descriptor, and what it said */
    uint32_t bad_type;
    uint64_t bad_start;
    uint64_t bad_end;
} n48_rt_res;

static inline uint32_t n48_rt_ld32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline uint64_t n48_rt_ld64(const uint8_t *p)
{
    return (uint64_t)n48_rt_ld32(p) | ((uint64_t)n48_rt_ld32(p + 4) << 32);
}

static inline void n48_rt_top_push(n48_rt_res *r, uint64_t start, uint64_t end, uint32_t type)
{
    uint32_t i, j;
    for (i = 0u; i < N48_RT_TOPS; i++) {
        if (i < r->tops && end <= r->top_end[i]) continue;
        for (j = N48_RT_TOPS - 1u; j > i; j--) {
            r->top_start[j] = r->top_start[j - 1u];
            r->top_end[j]   = r->top_end[j - 1u];
            r->top_type[j]  = r->top_type[j - 1u];
        }
        r->top_start[i] = start;
        r->top_end[i]   = end;
        r->top_type[i]  = type;
        if (r->tops < N48_RT_TOPS) r->tops++;
        return;
    }
}

// THE ONE ENTRY POINT. Returns the reason; `r->value` is the bound to use either way and is set on every path.
static inline uint32_t n48_rt_eval(const n48_rt_map *m, n48_rt_res *r)
{
    uint64_t prev_end = 0u, count = 0u, i;
    const uint64_t floor_v = (m && m->floor) ? m->floor : N48_RT_LEGACY_TOP;

    if (!r) return N48_RT_NO_MAP;
    {   /* no <string.h> in this header: the kext and the test both get the same explicit zeroing */
        n48_rt_res z;
        uint32_t k;
        z.reason = N48_RT_NO_MAP; z.value = floor_v; z.derived = 0u; z.floor = floor_v;
        z.descs = 0u; z.ram_descs = 0u; z.dev_descs = 0u; z.empty_descs = 0u; z.unsorted = 0u;
        z.ram_bytes = 0u; z.tops = 0u;
        z.bad_index = 0u; z.bad_type = 0u; z.bad_start = 0u; z.bad_end = 0u;
        for (k = 0u; k < N48_RT_TOPS; k++) { z.top_start[k] = 0u; z.top_end[k] = 0u; z.top_type[k] = 0u; }
        *r = z;
    }
    if (!m || !m->desc) return r->reason = N48_RT_NO_MAP, r->reason;

    if (m->desc_size < N48_RT_DESC_MIN || m->desc_size > N48_RT_DESC_MAX || (m->desc_size % 8u) != 0u)
        return r->reason = N48_RT_BAD_HEADER, r->reason;
    if (m->bytes == 0u || (m->bytes % (uint64_t)m->desc_size) != 0u)
        return r->reason = N48_RT_BAD_HEADER, r->reason;
    count = m->bytes / (uint64_t)m->desc_size;
    if (count == 0u || count > (uint64_t)N48_RT_DESC_COUNT_MAX)
        return r->reason = N48_RT_BAD_HEADER, r->reason;

    for (i = 0u; i < count; i++) {
        const uint8_t *p = m->desc + i * (uint64_t)m->desc_size;
        const uint32_t type  = n48_rt_ld32(p + N48_RT_OFF_TYPE);
        const uint64_t start = n48_rt_ld64(p + N48_RT_OFF_START);
        const uint64_t pages = n48_rt_ld64(p + N48_RT_OFF_PAGES);
        uint64_t end;

        r->descs++;
        r->bad_index = (uint32_t)i; r->bad_type = type; r->bad_start = start; r->bad_end = 0u;
        if (!n48_rt_type_ok(type))         return r->reason = N48_RT_BAD_DESC, r->reason;
        if ((start & (N48_RT_PAGE - 1u)) != 0u) return r->reason = N48_RT_BAD_DESC, r->reason;
        if (start >= N48_RT_ADDR_MAX)      return r->reason = N48_RT_BAD_DESC, r->reason;
        if (pages == 0u)                   { r->empty_descs++; continue; }
        if (pages > N48_RT_ADDR_MAX / N48_RT_PAGE) return r->reason = N48_RT_BAD_DESC, r->reason;
        end = start + pages * N48_RT_PAGE;
        r->bad_end = end;
        if (end <= start || end > N48_RT_ADDR_MAX) return r->reason = N48_RT_BAD_DESC, r->reason;

        if (start < prev_end) r->unsorted++;
        if (end > prev_end) prev_end = end;

        if (n48_rt_is_ram(type)) {
            r->ram_descs++;
            r->ram_bytes += pages * N48_RT_PAGE;
            if (end > r->derived) r->derived = end;
            n48_rt_top_push(r, start, end, type);
        } else if (n48_rt_is_device(type)) {
            r->dev_descs++;
        }
    }
    r->bad_index = 0u; r->bad_type = 0u; r->bad_start = 0u; r->bad_end = 0u;

    if (r->ram_descs == 0u || r->derived == 0u) return r->reason = N48_RT_NO_RAM, r->reason;

    // THE STRONG INTEGRITY CHECK. Generous downwards (firmware withdraws DRAM as Reserved/Unusable - TSEG and friends -
    // and those types are deliberately not counted), tight upwards (a map that claims MORE RAM than is installed is not
    // our map). Without PhysicalMemorySize we fall back to a shape test, which is much weaker and says so.
    if (m->phys_mem) {
        if (r->ram_bytes < m->phys_mem - m->phys_mem / 8u) return r->reason = N48_RT_TOTAL_MISMATCH, r->reason;
        if (r->ram_bytes > m->phys_mem + m->phys_mem / 64u) return r->reason = N48_RT_TOTAL_MISMATCH, r->reason;
    } else {
        if (r->ram_descs < 4u || r->ram_bytes < (1ull << 30)) return r->reason = N48_RT_TOTAL_MISMATCH, r->reason;
    }

    // THE REMAP CEILING - the total cross-check above constrains only the SUM of RAM descriptors, so a single small
    // RAM-type descriptor planted at a very high address moves the MAX END (`derived`) arbitrarily while the sum stays
    // inside tolerance. Independently of the sum, no DRAM can sit more than the whole sub-4 GiB PCI hole above the
    // installed total: that hole is the only DRAM the memory controller relocates above the physical top, and it is
    // smaller than 4 GiB by construction. A derived top beyond `phys_mem + 4 GiB` is therefore a descriptor that is
    // not this machine's RAM, and the refutation is the legacy constant. Skipped when PhysicalMemorySize is unknown
    // (m->phys_mem == 0), where only the weaker shape test above is available.
    if (m->phys_mem && r->derived > m->phys_mem + N48_RT_REMAP_MAX)
        return r->reason = N48_RT_TOP_ABOVE_PHYS, r->reason;

    // THE APERTURE PASS - the whole point of using the map. If any device window overlaps the band [4 GiB, derived)
    // that we are about to start accepting, this map does not mean what we think it means and we do not widen the
    // guard at all. The 64-bit PCI aperture sits above TOM2; a map in which it sits BELOW the top RAM descriptor is
    // either corrupt or a machine whose layout we have never seen, and either way the answer is the legacy constant.
    for (i = 0u; i < count; i++) {
        const uint8_t *p = m->desc + i * (uint64_t)m->desc_size;
        const uint32_t type  = n48_rt_ld32(p + N48_RT_OFF_TYPE);
        const uint64_t start = n48_rt_ld64(p + N48_RT_OFF_START);
        const uint64_t pages = n48_rt_ld64(p + N48_RT_OFF_PAGES);
        uint64_t end;
        if (pages == 0u || !n48_rt_is_device(type)) continue;
        end = start + pages * N48_RT_PAGE;
        if (start < r->derived && end > N48_RT_HOLE_HI) {
            r->bad_index = (uint32_t)i; r->bad_type = type; r->bad_start = start; r->bad_end = end;
            return r->reason = N48_RT_APERTURE, r->reason;
        }
    }

    r->reason = N48_RT_OK;
    r->value  = (r->derived < floor_v) ? floor_v : r->derived;
    return r->reason;
}

#ifdef __cplusplus
}
#endif

#endif /* N48_GFX_RAMTOP_H */
