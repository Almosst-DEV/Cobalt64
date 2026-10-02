// fbname_scan_test.cpp — prove the runtime class rename REFUSES rather than writes whenever the
// OSMetaClass layout is not exactly what it expects (0.0.308).
//
// Build and run on the host Mac (no kernel headers, no hardware, no PC):
//     clang++ -std=c++17 -Wall -Wextra -Werror -O1 -fsanitize=address,undefined \
//         -I src/navi48-bringup/src/dcn \
//         src/navi48-bringup/tests/fbname_scan_test.cpp -o /tmp/fbnametest && /tmp/fbnametest
//
// It compiles the SAME header the kext compiles. The property under test is the one that makes a
// live-kernel pointer write safe to attempt at all: a wrong offset must be a REFUSAL, never a write.

#include <cstdio>
#include <cstdint>
#include <cstring>

#include "fbname_scan.h"

static int checks, failures;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
    printf(__VA_ARGS__); printf("\n"); } } while (0)

// A plausible kernel pointer for the tests: canonical high-half, 8-byte aligned.
static uintptr_t kp(uintptr_t low) { return 0xffff000000000000ull | (low & ~0x7ull); }

int main(void)
{
    const uintptr_t SYM   = kp(0x1234568);     // the interned OSSymbol for "RDNA4FB"
    const uintptr_t SUPER = kp(0x2345678);     // the superclass OSMetaClass
    const uintptr_t VTBL  = kp(0x3456788);

    // ---- 1. the layout the SDK header describes: className is word 3 ----
    {
        uintptr_t w[N48_FBNAME_SCAN_WORDS] = { VTBL, 0, SUPER, SYM, 0x30, 0, 0, 0 };
        int idx = n48_fbname_find_unique(w, N48_FBNAME_SCAN_WORDS, SYM);
        CHECK(idx == 3, "className found at word %d, expected 3", idx);
        CHECK(n48_fbname_layout_ok(w, N48_FBNAME_SCAN_WORDS, idx), "layout accepted");
        CHECK(w[idx - 1] == SUPER, "the word before className is superClassLink");
    }

    // ---- 2. a DIFFERENT layout is still found, because the offset is searched not assumed ----
    {
        uintptr_t w[N48_FBNAME_SCAN_WORDS] = { VTBL, SUPER, SYM, 0, 0, 0, 0, 0 };
        int idx = n48_fbname_find_unique(w, N48_FBNAME_SCAN_WORDS, SYM);
        CHECK(idx == 2, "a kernel that orders the fields differently is still handled (idx %d)", idx);
        CHECK(n48_fbname_layout_ok(w, N48_FBNAME_SCAN_WORDS, idx), "and its layout is accepted");
    }

    // ---- 3. THE LOAD-BEARING REFUSALS ----
    {
        // absent: the symbol is not in the structure at all
        uintptr_t w[N48_FBNAME_SCAN_WORDS] = { VTBL, 0, SUPER, kp(0x999), 0, 0, 0, 0 };
        CHECK(n48_fbname_find_unique(w, N48_FBNAME_SCAN_WORDS, SYM) == N48_FBNAME_FOUND_NONE,
              "a layout with no matching word REFUSES");
    }
    {
        // ambiguous: two words hold it, so which one is className is not decidable
        uintptr_t w[N48_FBNAME_SCAN_WORDS] = { VTBL, SYM, SUPER, SYM, 0, 0, 0, 0 };
        CHECK(n48_fbname_find_unique(w, N48_FBNAME_SCAN_WORDS, SYM) == N48_FBNAME_FOUND_AMBIGUOUS,
              "two matching words REFUSE rather than pick one");
    }
    {
        // className appearing to be word 0 would mean we had found the vtable pointer
        uintptr_t w[N48_FBNAME_SCAN_WORDS] = { SYM, 0, 0, 0, 0, 0, 0, 0 };
        int idx = n48_fbname_find_unique(w, N48_FBNAME_SCAN_WORDS, SYM);
        CHECK(idx == 0, "word 0 is found...");
        CHECK(!n48_fbname_layout_ok(w, N48_FBNAME_SCAN_WORDS, idx),
              "...but REFUSED, because word 0 is the vtable pointer, not className");
    }
    {
        // the word before className is not a pointer -> not superClassLink -> refuse
        uintptr_t w[N48_FBNAME_SCAN_WORDS] = { VTBL, 0, 0x30, SYM, 0, 0, 0, 0 };
        int idx = n48_fbname_find_unique(w, N48_FBNAME_SCAN_WORDS, SYM);
        CHECK(idx == 3, "found at 3");
        CHECK(!n48_fbname_layout_ok(w, N48_FBNAME_SCAN_WORDS, idx),
              "but REFUSED: the preceding word is not a plausible superClassLink");
    }

    // ---- 4. argument guards, none of which may be a write ----
    {
        uintptr_t w[N48_FBNAME_SCAN_WORDS] = { VTBL, 0, SUPER, SYM, 0, 0, 0, 0 };
        CHECK(n48_fbname_find_unique(nullptr, N48_FBNAME_SCAN_WORDS, SYM) == N48_FBNAME_BAD_ARG, "NULL words");
        CHECK(n48_fbname_find_unique(w, 0, SYM) == N48_FBNAME_BAD_ARG, "zero count");
        CHECK(n48_fbname_find_unique(w, N48_FBNAME_SCAN_WORDS + 1, SYM) == N48_FBNAME_BAD_ARG,
              "a count past the scan window is refused");
        CHECK(n48_fbname_find_unique(w, N48_FBNAME_SCAN_WORDS, 0) == N48_FBNAME_BAD_ARG, "null target");
        CHECK(!n48_fbname_layout_ok(nullptr, N48_FBNAME_SCAN_WORDS, 3), "NULL words in layout_ok");
        CHECK(!n48_fbname_layout_ok(w, N48_FBNAME_SCAN_WORDS, -1), "negative index");
        CHECK(!n48_fbname_layout_ok(w, N48_FBNAME_SCAN_WORDS, (int)N48_FBNAME_SCAN_WORDS),
              "index at the end of the window");
    }

    // ---- 5. the pointer plausibility filter ----
    CHECK(n48_fbname_plausible_kptr(kp(0x1000)), "a canonical aligned kernel pointer is plausible");
    CHECK(!n48_fbname_plausible_kptr(0), "NULL is not");
    CHECK(!n48_fbname_plausible_kptr(kp(0x1000) | 4), "a misaligned pointer is not");
    CHECK(!n48_fbname_plausible_kptr(0x0000700000001000ull), "a userspace address is not");
    CHECK(!n48_fbname_plausible_kptr(0x30), "a small integer (a size field) is not");
    CHECK(!n48_fbname_plausible_kptr(0xfffe000000001000ull), "a non-canonical high half is not");

    printf("fbname_scan_test: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
