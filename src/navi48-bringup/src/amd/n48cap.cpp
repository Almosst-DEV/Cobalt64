#include "n48cap.h"
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <kern/clock.h>

namespace amdgpu {

namespace {
char         *g_cbuf     = nullptr;
uint32_t      g_csize    = 0;
uint32_t      g_cused    = 0;
uint32_t      g_cseq     = 0;
uint64_t      g_cconsumed = 0;
uint64_t      g_cdropped  = 0;
uint64_t      g_crecords  = 0;
IOSimpleLock *g_clock    = nullptr;
} // namespace

bool n48_cap_alloc(uint32_t bytes)
{
    if (g_cbuf) return true;
    if (bytes < 65536u) bytes = 65536u;
    bytes &= ~3u;
    if (!g_clock) g_clock = IOSimpleLockAlloc();
    if (!g_clock) return false;
    char *b = static_cast<char *>(IOMalloc(bytes));
    if (!b) return false;
    IOInterruptState s = IOSimpleLockLockDisableInterrupt(g_clock);
    if (!g_cbuf) { g_cbuf = b; g_csize = bytes; g_cused = 0; b = nullptr; }
    IOSimpleLockUnlockEnableInterrupt(g_clock, s);
    if (b) IOFree(b, bytes);
    return true;
}

bool n48_cap_ready(void) { return g_cbuf != nullptr; }

bool n48_cap_append(uint32_t type, const void *hdr, uint32_t hdrLen, const void *body, uint32_t bodyLen)
{
    if (!g_cbuf || !g_clock) { g_cdropped += (uint64_t)kN48CapHeaderSize + hdrLen + bodyLen; return false; }
    const uint64_t raw = (uint64_t)kN48CapHeaderSize + hdrLen + bodyLen;
    const uint64_t total = (raw + 3u) & ~3ull;
    if (total > 0x7fffffffull) { g_cdropped += raw; return false; }
    uint64_t now = 0;
    clock_get_uptime(&now);
    bool ok = false;
    IOInterruptState s = IOSimpleLockLockDisableInterrupt(g_clock);
    if ((uint64_t)g_cused + total <= g_csize) {
        char *p = g_cbuf + g_cused;
        const uint32_t h[4] = { kN48CapMagic, type, (uint32_t)total, ++g_cseq };
        memcpy(p, h, sizeof(h));
        memcpy(p + 16, &now, 8);
        if (hdrLen) memcpy(p + kN48CapHeaderSize, hdr, hdrLen);
        if (bodyLen) memcpy(p + kN48CapHeaderSize + hdrLen, body, bodyLen);
        for (uint64_t i = raw; i < total; i++) p[i] = 0;
        g_cused += (uint32_t)total;
        g_crecords++;
        ok = true;
    } else {
        g_cdropped += total;
    }
    IOSimpleLockUnlockEnableInterrupt(g_clock, s);
    return ok;
}

uint32_t n48_cap_read(uint32_t offset, void *dst, uint32_t max, uint32_t *total)
{
    if (!g_cbuf || !g_clock) { if (total) *total = 0; return 0; }
    if (dst == nullptr || max == 0) { if (total) *total = g_cused; return 0; }
    uint32_t copied = 0;
    IOInterruptState s = IOSimpleLockLockDisableInterrupt(g_clock);
    if (total) *total = g_cused;
    if (offset < g_cused) {
        copied = g_cused - offset;
        if (copied > max) copied = max;
        memcpy(dst, g_cbuf + offset, copied);
    }
    IOSimpleLockUnlockEnableInterrupt(g_clock, s);
    return copied;
}

uint32_t n48_cap_consume(uint32_t upTo, uint64_t *consumedTotal, uint64_t *droppedTotal, uint64_t *recordsTotal)
{
    uint32_t held = 0;
    if (g_cbuf && g_clock) {
        IOInterruptState s = IOSimpleLockLockDisableInterrupt(g_clock);
        if (upTo > g_cused) upTo = g_cused;
        if (upTo) {
            memmove(g_cbuf, g_cbuf + upTo, g_cused - upTo);
            g_cused -= upTo;
            g_cconsumed += upTo;
        }
        held = g_cused;
        IOSimpleLockUnlockEnableInterrupt(g_clock, s);
    }
    if (consumedTotal) *consumedTotal = g_cconsumed;
    if (droppedTotal) *droppedTotal = g_cdropped;
    if (recordsTotal) *recordsTotal = g_crecords;
    return held;
}

} // namespace amdgpu
