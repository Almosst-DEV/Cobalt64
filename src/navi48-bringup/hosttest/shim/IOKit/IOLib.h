//
//  hosttest/shim/IOKit/IOLib.h — minimal host-userspace stand-in for the
//  kernel KPI header <IOKit/IOLib.h> pulled in (transitively) by
//  src/amd/amdgpu_regs.h. See IOReturn.h in this same directory for the
//  scope/rationale — this shim exists solely so hosttest/ can compile
//  amdgpu_ucode_extract.cpp (and the headers it drags in) with a plain host
//  clang++, without touching anything under src/.
//
//  Real signatures (from the kernel KPI), reproduced here as trivial host
//  wrappers. amdgpu_ucode_extract() itself never calls any of these — it has
//  zero logging/sleep/alloc call sites — but amdgpu_regs.h's other inline
//  helpers (RREG32, poll_psp_response, WBAR0_32, ...) reference them, and
//  since those helpers are `static inline` and physically present in this
//  translation unit via the #include chain, they must at least be
//  well-formed declarations for the header to parse and type-check cleanly.
//
#pragma once

#include <stddef.h>   // size_t — amdgpu_regs.h declares extern "C" memcpy(...,size_t) before <string.h> is seen
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// IOLog(const char *format, ...) -> vprintf, per the task's shim spec.
static inline void IOLog(const char *format, ...) {
    va_list ap;
    va_start(ap, format);
    vprintf(format, ap);
    va_end(ap);
}

// IOSleep(unsigned milliseconds) -> usleep.
static inline void IOSleep(unsigned milliseconds) {
    usleep(milliseconds * 1000u);
}

// IOMalloc/IOFree -> malloc/free (kernel signature takes a size on free too;
// the host allocator doesn't need it, so it's just ignored).
static inline void *IOMalloc(size_t size) {
    return malloc(size);
}
static inline void IOFree(void *address, size_t size) {
    (void)size;
    free(address);
}

// bzero(void*, size_t) — BSD legacy zeroing helper some kernel code assumes
// is ambiently available. On the real kernel KPI this is a freestanding
// builtin; on the host, Darwin's own <string.h> (included above) already
// transitively declares a fully working `bzero` via <_strings.h>, so there
// is nothing to shim here — declaring our own would conflict with libc's
// non-static declaration (tried it: "static declaration of 'bzero' follows
// non-static declaration"). Left as a comment so the intent is documented.
