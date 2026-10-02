//
//  n48log.h — the driver's own log ring.
//
//  Everything the bring-up code prints has gone through IOLog and been read
//  back with `log show`. That stopped working once the kext moved into the
//  boot kernel collection (OpenCore injection): the messages are simply not in
//  the unified log any more, at any level, while the driver is demonstrably
//  running (the IORegistry properties it publishes are all there). Worse, even
//  when it did work, `log show` drops kernel debug entries from the *previous*
//  boot, so a crash that took the machine down took its evidence with it.
//
//  So the driver keeps its own copy: a fixed circular buffer written by every
//  log macro, readable through the user client (`navi48test log`) with no
//  dependency on macOS logging at all. IOLog is still called, so nothing is
//  lost when the unified log does work.
//
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "../Navi48UserClientABI.h"   // 0.0.263: NAVI48_LOG_BYTES, shared with navi48test

namespace amdgpu {

// 0.0.263: sized from the SHARED constant, because navi48test independently
// hardcoded the same figure to decide when to warn "AT CAPACITY" and the two could drift.
// 512 KiB was not enough: r96 and r97 both filled it mid-run and dropped their late-boot
// lines, so neither log could be claimed complete.
constexpr uint32_t kN48LogBytes = NAVI48_LOG_BYTES;

// Append one formatted line (a newline is added). Safe from any context that
// may take a spin lock — the interrupt handler's work-loop callback included.
void n48_logf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

// Copy out at most `max` bytes starting at `offset` into the ring's linear
// (oldest-first) view. Returns bytes copied; sets *total to the number of
// bytes currently held. Reading never disturbs the ring.
uint32_t n48_log_read(uint32_t offset, void *dst, uint32_t max, uint32_t *total);

// Drop everything. Used before a test so its output stands alone.
void n48_log_reset(void);

// True once the buffer filled and began DISCARDING new lines. Tracked since the
// log was written but never surfaced, so a truncated buffer read back as though
// it were complete.
bool n48_log_overflowed(void);

// 0.0.276: drop the first `upTo` bytes (clamped to what is held), shifting the rest down. Returns the
// bytes still held; reports the running totals of consumed and of dropped-while-full bytes.
uint32_t n48_log_consume(uint32_t upTo, uint64_t *consumedTotal, uint64_t *droppedTotal);

} // namespace amdgpu
