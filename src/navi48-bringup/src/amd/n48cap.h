//
//  n48cap.h — the binary CAPTURE ring (0.0.282).
//
//  The driver log carries lines of at most 512 characters and is shared with every instrument; a per-frame capture of a
//  client's GPU work (a 3744-dword IB, its programs, its descriptor tables) would be ~40 KB of hex per frame there and
//  would crowd out the lines the run is judged by. This is a second, binary, append-only buffer with the same streaming
//  contract as the log (0.0.276): a reader copies, writes to disk, then consumes what it wrote, so a panic loses at most
//  the last interval (rule 92).
//
//  Records are appended whole under one spin lock (a record either fits or is dropped and counted), so a reader that
//  reads until the offset reaches the held size always stops on a record boundary.
//
//  Record layout (little-endian, 4-byte aligned):
//    +0x00 u32 magic 0x5243344e ("N4CR")   +0x04 u32 type   +0x08 u32 total bytes incl. this 24-byte header and padding
//    +0x0c u32 sequence (per ring)          +0x10 u64 clock_get_uptime() at append
//    +0x18 payload (the caller's header bytes, then its body bytes), zero-padded to 4
//
#pragma once
#include <stdint.h>
#include <stddef.h>

namespace amdgpu {

constexpr uint32_t kN48CapMagic      = 0x5243344eu;
constexpr uint32_t kN48CapHeaderSize = 24u;

// Allocate the ring once (idempotent; a second call with any size returns true if a ring exists). Thread context only.
bool n48_cap_alloc(uint32_t bytes);
bool n48_cap_ready(void);

// Append one record: header bytes then body bytes. Returns true if the whole record was stored, false (and counts the
// bytes as dropped) if the ring lacks room or has not been allocated.
bool n48_cap_append(uint32_t type, const void *hdr, uint32_t hdrLen, const void *body, uint32_t bodyLen);

// As n48_log_read / n48_log_consume.
uint32_t n48_cap_read(uint32_t offset, void *dst, uint32_t max, uint32_t *total);
uint32_t n48_cap_consume(uint32_t upTo, uint64_t *consumedTotal, uint64_t *droppedTotal, uint64_t *recordsTotal);

} // namespace amdgpu
