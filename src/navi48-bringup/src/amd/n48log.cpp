#include "n48log.h"
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <stdarg.h>
#include <kern/clock.h>

// build 0.0.540 (; apple/gfx_perf540.h) — SWITCH 96, perf540. Defined in Navi48Bringup.cpp (declared for the kext
// in apple/Navi48Ttl.hpp): the switch, and the note of this function's three phases by thread class. OFF: one load, no clock.
extern volatile uint32_t gN48Pf540On;
void navi48_pf540_log_note(uint64_t t0, uint64_t t1, uint64_t t2, uint64_t t3);

namespace amdgpu {

namespace {
// Linear buffer plus a write cursor. When it fills we stop appending rather
// than wrapping: for bring-up the FIRST failure is what matters, and a wrap
// would throw away the ladder's early stages, which are usually the context
// that explains a late failure.
char      g_buf[kN48LogBytes];
uint32_t  g_used      = 0;
bool      g_overflow  = false;
uint64_t  g_consumed  = 0;   // 0.0.276: bytes a streaming reader has taken out
uint64_t  g_dropped   = 0;   // 0.0.276: bytes refused while full
IOSimpleLock *g_lock  = nullptr;

IOSimpleLock *lock() {
    if (!g_lock) g_lock = IOSimpleLockAlloc();   // first call is single-threaded (kext start)
    return g_lock;
}
} // namespace

void n48_logf(const char *fmt, ...)
{
    char line[512];
    // build 0.0.540 (switch 96, T9): read ONCE; OFF no clock is read and nothing below changes.
    const uint32_t pf = __atomic_load_n(&gN48Pf540On, __ATOMIC_RELAXED);
    uint64_t pt0 = 0ull, pt1 = 0ull, pt2 = 0ull, pt3 = 0ull;
    if (pf) clock_get_uptime(&pt0);
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (n < 0) return;
    uint32_t len = (uint32_t)((size_t)n < sizeof(line) - 1 ? (size_t)n : sizeof(line) - 1);
    if (pf) clock_get_uptime(&pt1);

    // Keep IOLog too: when the unified log does work it is still the most
    // convenient place to watch a run live.
    IOLog("%s", line);
    if (pf) clock_get_uptime(&pt2);

    IOSimpleLock *l = lock();
    if (!l) return;
    IOInterruptState s = IOSimpleLockLockDisableInterrupt(l);
    if (g_used + len + 1 <= kN48LogBytes) {
        memcpy(g_buf + g_used, line, len);
        g_used += len;
        if (g_used == 0 || g_buf[g_used - 1] != '\n') g_buf[g_used++] = '\n';
    } else {
        g_overflow = true;
        g_dropped += len + 1;
    }
    IOSimpleLockUnlockEnableInterrupt(l, s);
    if (pf) { clock_get_uptime(&pt3); navi48_pf540_log_note(pt0, pt1, pt2, pt3); }
}

bool n48_log_overflowed(void) { return g_overflow; }

uint32_t n48_log_read(uint32_t offset, void *dst, uint32_t max, uint32_t *total)
{
    if (dst == nullptr || max == 0) { if (total) *total = g_used; return 0; }
    IOSimpleLock *l = lock();
    uint32_t copied = 0;
    IOInterruptState s = IOSimpleLockLockDisableInterrupt(l);
    if (total) *total = g_used;
    if (offset < g_used) {
        copied = g_used - offset;
        if (copied > max) copied = max;
        memcpy(dst, g_buf + offset, copied);
    }
    IOSimpleLockUnlockEnableInterrupt(l, s);
    return copied;
}

uint32_t n48_log_consume(uint32_t upTo, uint64_t *consumedTotal, uint64_t *droppedTotal)
{
    IOSimpleLock *l = lock();
    IOInterruptState s = IOSimpleLockLockDisableInterrupt(l);
    if (upTo > g_used) upTo = g_used;
    if (upTo) {
        // Only what arrived after the reader's copy moves, which is small at a 1 s cadence.
        memmove(g_buf, g_buf + upTo, g_used - upTo);
        g_used -= upTo;
        g_consumed += upTo;
        g_overflow = false;
    }
    const uint32_t held = g_used;
    if (consumedTotal) *consumedTotal = g_consumed;
    if (droppedTotal) *droppedTotal = g_dropped;
    IOSimpleLockUnlockEnableInterrupt(l, s);
    return held;
}

void n48_log_reset(void)
{
    IOSimpleLock *l = lock();
    IOInterruptState s = IOSimpleLockLockDisableInterrupt(l);
    g_used = 0; g_overflow = false;
    IOSimpleLockUnlockEnableInterrupt(l, s);
}

} // namespace amdgpu
