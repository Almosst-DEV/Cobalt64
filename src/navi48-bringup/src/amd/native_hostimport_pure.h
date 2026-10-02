//
//  native_hostimport_pure.h - the pure half of BoImportHost (ABI 1.9, kext 0.0.612, milestone #11 step 11c; notes/design/NATIVE-S4-M11.md sections 3 (Q2) and 4 (Q3)). No kernel
//  header: tests/native_hostimport_test.cpp compiles this very file and drives it; n1c_bo_import_host / bo_release in native_s1c.cpp are the callers.
//
//  What BoImportHost is: the caller (WindowServer, through the bundle) hands the kernel a page-aligned range of ITS OWN address space (an IOSurface's base address); the kernel wires
//  it (IOMemoryDescriptor::withAddressRange(task) + prepare()), records the physical page list (scattered: one entry per 4 KiB page) and maps those pages into VMID 8 as SYSTEM entries.
//  Mapping is done either by the ordinary GemVa MAP on the returned handle (the path RADV's buffer_from_ptr takes: RADV owns the VA space) or, when the caller passes a GPU VA, at import.
//
//  Limits (the brief): host va and size 4 KiB aligned, size non-zero, size <= 64 MiB per BO, total imported <= 2 GiB per client (0.0.620; was 256 MiB: WindowServer's IOSurface backing stores reached it in normal use; a cap SEPARATE from the 512 MiB GTT cap).
//
#pragma once
#include <stdint.h>

#include "native_s1c_pure.h"

namespace n48native {
namespace s1c {

constexpr uint64_t kImportMaxBo   = 64ull << 20;                 // per BO
constexpr uint64_t kImportCap     = 2048ull << 20;               // per client, 2 GiB (0.0.620), separate from kGttCap (512 MiB)
constexpr uint64_t kImportUserMax = 1ull << 47;                  // the x86-64 user half: a host range must end at or below it
constexpr uint32_t kImportMaxPages = (uint32_t)(kImportMaxBo / kPage);   // 16384: the page list of one BO (128 KiB)
// The flags word is the GemVa vm-flags subset that matters for a leaf: R / W / X and the MTYPE. Non-zero only together with a GPU VA (map at import).
constexpr uint32_t kImportVmMask  = N48N_VM_PAGE_READABLE | N48N_VM_PAGE_WRITEABLE | N48N_VM_PAGE_EXECUTABLE | N48N_VM_MTYPE_MASK;
static_assert(kImportMaxPages == 16384u, "64 MiB / 4 KiB");
static_assert(kImportCap > kImportMaxBo && kImportCap != kGttCap, "the import cap is its own cap");

struct ImportChk { uint32_t rc; uint64_t pages; uint64_t gpuStripped; };   // gpuStripped: the 48-bit VA to map at (0 = no map at import)
// Everything that can be judged from the arguments and the running total. `importedNow` = bytes this client has imported and not freed.
// Order: malformed arguments (BadArg) first, the cap last (NoMemory), like BoCreate's placement then its pool.
constexpr ImportChk import_check(uint64_t hostVa, uint64_t size, uint64_t flags, uint64_t gpuVa, uint64_t importedNow) {
    if (size == 0ull || hostVa == 0ull) return ImportChk{ kBadArg, 0, 0 };
    if (((hostVa | size) & (kPage - 1ull)) != 0ull) return ImportChk{ kBadArg, 0, 0 };
    if (hostVa >= kImportUserMax || size > kImportUserMax - hostVa) return ImportChk{ kBadArg, 0, 0 };   // no wrap, user half only
    if (size > kImportMaxBo) return ImportChk{ kBadArg, 0, 0 };
    if ((flags >> 32) != 0ull || (((uint32_t)flags) & ~kImportVmMask) != 0u || !vm_mtype_known((uint32_t)flags)) return ImportChk{ kBadArg, 0, 0 };
    uint64_t stripped = 0ull;
    if (gpuVa == 0ull) {
        if (flags != 0ull) return ImportChk{ kBadArg, 0, 0 };                        // flags mean nothing without a VA to map at
    } else {
        if ((((uint32_t)flags) & (N48N_VM_PAGE_READABLE | N48N_VM_PAGE_WRITEABLE)) == 0u) return ImportChk{ kBadArg, 0, 0 };
        const VaReq q = gemva_check(N48N_VA_OP_MAP, 1u, 0u, (uint32_t)flags, gpuVa, 0ull, size, 0ull, 0u, 0u, 0ull);
        if (q.rc != kOk) return ImportChk{ q.rc, 0, 0 };
        stripped = q.stripped;
    }
    if (would_exceed(importedNow, size, kImportCap)) return ImportChk{ kNoMemory, 0, 0 };
    return ImportChk{ kOk, size / kPage, stripped };
}
// The running total after a free (never below zero: a stray double free must not wrap the cap open).
constexpr uint64_t import_after_free(uint64_t importedNow, uint64_t size) { return importedNow >= size ? importedNow - size : 0ull; }

// ---- the physical page list ---------------------------------------------------------------------------------------------------------
// `Seg` supplies: uint64_t phys(uint64_t off, uint64_t *len) -> the physical address of byte `off` of the (prepared) descriptor and the length of the physically contiguous run there
// (0 for no such byte). Fills out[0 .. size/4096) with one 4 KiB physical page address each. kOk / kNoMemory (a missing, short or unaligned run) / kBadArg (an address the PTE cannot hold,
// or a count that does not match the size).
template <class Seg>
inline uint32_t collect_pages(Seg &s, uint64_t size, uint64_t *out, uint64_t cap) {
    if (size == 0ull || (size & (kPage - 1ull)) != 0ull) return kBadArg;
    const uint64_t pages = size / kPage;
    if (pages > cap) return kBadArg;
    uint64_t n = 0, off = 0;
    while (off < size) {
        uint64_t len = 0;
        const uint64_t pa = s.phys(off, &len);
        if (pa == 0ull || len < kPage || (pa & (kPage - 1ull)) != 0ull) return kNoMemory;
        uint64_t take = (len < size - off ? len : size - off) / kPage;   // whole pages of this run that belong to the range
        for (uint64_t k = 0; k < take; k++) {
            const uint64_t p = pa + k * kPage;
            if ((p & ~kPtePaMask) != 0ull) return kBadArg;               // a physical page above the PTE's address field
            out[n++] = p;
        }
        off += take * kPage;
    }
    return n == pages ? kOk : kBadArg;
}

// Map `pages` 4 KiB pages at `va` to the pages named by pagePa[0 .. pages) (SCATTERED: pt_map's consecutive-run twin). Same contract as pt_map: every leaf is validated first, then every
// table page is ensured (the only step that can fail for space), then the leaves are written, so a failed call leaves no partial mapping. kOk / kNoMemory / kBadArg.
template <class M>
inline uint32_t pt_map_pages(uint64_t rootPa, uint64_t va, uint64_t pages, const uint64_t *pagePa, uint64_t leafFlagsIn, M &m) {
    if (pages == 0ull || pagePa == nullptr) return kBadArg;
    for (uint64_t i = 0; i < pages; i++) if (pte_encode(pagePa[i], leafFlagsIn) == 0ull || (pagePa[i] & (kPage - 1ull)) != 0ull) return kBadArg;
    for (uint64_t p = 0; p < pages;) {
        const uint64_t v = va + p * kPage;
        if (pt_ensure_ptb(rootPa, v, m) == 0ull) return kNoMemory;
        p += 512ull - idx_ptb(v);                                      // to the next PTB
    }
    for (uint64_t p = 0; p < pages;) {
        const uint64_t v = va + p * kPage;
        const uint64_t ptb = pt_ensure_ptb(rootPa, v, m);              // all present now: no allocation
        if (ptb == 0ull) return kNoMemory;
        const uint64_t run = 512ull - idx_ptb(v) < pages - p ? 512ull - idx_ptb(v) : pages - p;
        for (uint64_t k = 0; k < run; k++) m.wr(ptb, idx_ptb(v) + (uint32_t)k, pte_encode(pagePa[p + k], leafFlagsIn));
        p += run;
    }
    return kOk;
}

// ---- device memory refusal (0.0.612 review item A) -----------------------------------------------------------------------------------------
// A caller could wire a range of ITS OWN address space that is really device memory (a mapped PCI BAR: ours, or another device's), and BoImportHost would then hand the GPU a PTE that
// reads or writes that device. So every physical page of the list is judged: refused when it overlaps ANY of this GPU's PCI BARs (the FULL sizes read from the IOPCIDevice, all
// six BARs and the expansion ROM, 64-bit BARs already joined by the kernel into one base) and, when the kext knows the top of DRAM, when it lies at or above that top.
struct PciBar { uint64_t base; uint64_t size; };            // size 0 = the BAR is absent / unassigned (skipped)
constexpr uint32_t kMaxPciBars = 7;                          // BAR0..BAR5 + the expansion ROM
enum PageVerdict : uint32_t { kPageOk = 0, kPageNoBars = 1, kPageInBar = 2, kPageAboveDram = 3 };
// pa: a 4 KiB page address. bars / nbars: the latched BARs (nbars == 0: nothing was latched, so the BAR question cannot be answered: FAIL CLOSED). dramTop: the first byte above DRAM,
// 0 = unknown (then only the BAR test applies). A page is judged as the whole [pa, pa + 4 KiB) so an unaligned BAR base can never let a partly covered page through.
constexpr PageVerdict import_page_verdict(uint64_t pa, const PciBar *bars, uint32_t nbars, uint64_t dramTop) {
    if (bars == nullptr || nbars == 0u) return kPageNoBars;
    const uint64_t pe = pa + kPage;
    if (pe < pa) return kPageInBar;                          // wraps the address space: never DRAM
    for (uint32_t i = 0; i < nbars; i++) {
        if (bars[i].size == 0ull) continue;
        const uint64_t be = bars[i].base + bars[i].size;
        const uint64_t end = be < bars[i].base ? ~0ull : be;   // a wrapping BAR is read as reaching the top of the address space (refuses more, never less)
        if (pa < end && pe > bars[i].base) return kPageInBar;
    }
    if (dramTop != 0ull && pe > dramTop) return kPageAboveDram;
    return kPageOk;
}
constexpr bool import_page_allowed(uint64_t pa, const PciBar *bars, uint32_t nbars, uint64_t dramTop) { return import_page_verdict(pa, bars, nbars, dramTop) == kPageOk; }
// The index of the first refused page of pagePa[0 .. n), or n when every page is allowed.
constexpr uint64_t import_first_refused(const uint64_t *pagePa, uint64_t n, const PciBar *bars, uint32_t nbars, uint64_t dramTop) {
    for (uint64_t i = 0; i < n; i++) if (!import_page_allowed(pagePa[i], bars, nbars, dramTop)) return i;
    return n;
}

// ---- session identity (0.0.612 review item C) ---------------------------------------------------------------------------------------------
// The import wires the caller's range BEFORE it takes the client lock. The session sequence read at entry (0 = no session) must still be the current one under the lock: a close, or a
// close and a new open, while the wiring slept means these pages belong to a client that is gone.
constexpr bool session_unchanged(uint32_t seqAtEntry, uint32_t seqNow) { return seqAtEntry != 0u && seqAtEntry == seqNow; }

// ---- release ------------------------------------------------------------------------------------------------------------------------
// The release modes of native_s1c.cpp (RelMode; the kext static_asserts these equal it).
constexpr uint32_t kHostRelNormal = 0u, kHostRelLeak = 1u, kHostRelClosing = 2u;
// complete() + release() of the imported pages: NEVER in the HUNG leak mode (the GPU might still read them: the pages, the descriptor and the page list are leaked, exactly like the other BOs
// n1c_close leaks); in Normal only after the PTEs are cleared and the TLB flushed; in Closing only after CONTEXT8 is parked (program_root(park) flushes the TLB).
constexpr bool host_may_release(uint32_t mode) { return mode == kHostRelNormal || mode == kHostRelClosing; }
// 0.0.612 review item D: a TLB flush that did not acknowledge (flush_vmid returned non-zero: it latched HUNG) means the GPU may still translate through the entries just cleared, so
// the memory of that BO (the imported host pages, and the GTT pages) is LEAKED, never completed / released / freed.
constexpr bool memory_may_free_after_flush(uint32_t flushRc) { return flushRc == 0u; }

} // namespace s1c
} // namespace n48native
