//
//  fbname_scan.h — the pure, host-testable core of the runtime class rename.
//
//  WHAT THE RENAME IS FOR
//
//  CoreDisplay resolves a framebuffer's GPU vendor by a case-sensitive substring search on its IOKit
//  class name (notes/M4-CAPABILITIES-STRUCT.md). Our framebuffer publishes as `RDNA4FB`, matches
//  none of "Intel"/"AMD"/"AppleParavirt", takes the unknown-vendor bit 0x1, and that bit cannot be
//  covered by any preference — so `UseIOPresentment()` is false and the desktop is CPU-composited.
//  The compile-time fix is blocked: the kext installs through the Auxiliary collection, which cannot
//  be rebuilt on this machine without a KDK. This is the runtime alternative: the name lives in
//  ONE pointer inside the class's `OSMetaClass`, and our already-loaded kext can replace it.
//
//  WHY THE SEARCH, RATHER THAN A CONSTANT OFFSET
//
//  MacKernelSDK's OSMetaClass.h puts `className` third among the fields after the vtable pointer
//  (`reserved`, `superClassLink`, `className`), i.e. +0x18 on x86_64. **That header is not this
//  kernel.** Writing a pointer at an offset taken from a header, into a live kernel structure, is
//  exactly the class of mistake that corrupts a machine instead of refusing. So the offset is never
//  assumed: it is FOUND, and the find must be unique.
//
//  OSSymbol is interned — `OSSymbol::withCString("RDNA4FB")` returns the very same object the
//  metaclass holds — so the caller can obtain the exact pointer to look for through public KPI, with
//  no dereference of anything unverified, and then search the metaclass words for it.
//
//  THE RULE THIS FILE ENFORCES: exactly one word may match. Zero means the layout is not what we
//  think and nothing is written. Two or more means the match is ambiguous and nothing is written.
//  A wrong offset is a REFUSAL, never a write.
//
#ifndef Navi48FbnameScan_h
#define Navi48FbnameScan_h

#include <stdint.h>
#include <stddef.h>

// How many machine words of the OSMetaClass to consider. className is expected at word 3
// (vtable, reserved, superClassLink, className); 8 words = 64 bytes stays well inside the object.
#define N48_FBNAME_SCAN_WORDS 8u

enum {
    N48_FBNAME_FOUND_NONE      = -1,   // no word held the symbol: the layout is not what we expect
    N48_FBNAME_FOUND_AMBIGUOUS = -2,   // more than one did: which is `className` is not decidable
    N48_FBNAME_BAD_ARG         = -3,
};

// Index of the UNIQUE word equal to `target`, or one of the negative codes above.
// Pure: no dereference of `target`, no allocation, no side effects.
static inline int n48_fbname_find_unique(const uintptr_t *words, unsigned count, uintptr_t target)
{
    if (!words || count == 0 || count > N48_FBNAME_SCAN_WORDS || target == 0)
        return N48_FBNAME_BAD_ARG;
    int found = N48_FBNAME_FOUND_NONE;
    for (unsigned i = 0; i < count; i++) {
        if (words[i] != target)
            continue;
        if (found >= 0)
            return N48_FBNAME_FOUND_AMBIGUOUS;
        found = (int)i;
    }
    return found;
}

// A kernel pointer this kext is willing to dereference: canonical, high-half, aligned, not obviously
// a small integer. Deliberately conservative — the scan must never turn a stale word into a fault.
static inline int n48_fbname_plausible_kptr(uintptr_t p)
{
    if (p == 0) return 0;
    if (p & 0x7u) return 0;                                  // 8-byte aligned
    if ((p >> 48) != 0xffffu) return 0;                      // x86_64 kernel half
    return 1;
}

// Is this the layout we expect? `className` must not be word 0 (that is the vtable pointer), and the
// word immediately before it must be a plausible pointer — `superClassLink`, which the caller then
// confirms by asking it for its own class name. Encodes the ONE structural assumption we make and
// makes it checkable.
static inline int n48_fbname_layout_ok(const uintptr_t *words, unsigned count, int idx)
{
    if (!words || idx <= 0 || (unsigned)idx >= count)
        return 0;
    return n48_fbname_plausible_kptr(words[idx - 1]);
}

#endif /* Navi48FbnameScan_h */
