//
//  Navi48UserClientABI.h — the kernel/userspace contract for Navi48Bringup.
//
//  Included by BOTH the kext (src/Navi48UserClient.cpp) and the userspace
//  harness (tools/pc/navi48test.c), so there is exactly one definition of
//  every selector and struct. Plain C, no IOKit types.
//
//  Stability: none. This is a bring-up interface for our own test tool; the
//  selector numbers change whenever it is convenient. The real accelerator
//  interface (Phase 4) is a different, Apple-shaped thing entirely.
//
#ifndef Navi48UserClientABI_h
#define Navi48UserClientABI_h

#include <stdint.h>

#define NAVI48_UC_SERVICE_NAME "Navi48Bringup"
#define NAVI48_UC_ABI_VERSION  6u

enum Navi48Selector {
    kNavi48SelGetInfo     = 0,   // out struct Navi48Info
    kNavi48SelAllocVRAM   = 1,   // in: size, align[, flags]  out: gpu_va, handle
    kNavi48SelFreeVRAM    = 2,   // in: handle
    kNavi48SelWriteVRAM   = 3,   // in: gpu_va + struct bytes
    kNavi48SelReadVRAM    = 4,   // in: gpu_va, len        out struct bytes
    kNavi48SelSubmitIB    = 5,   // in: gpu_va, len_dw, vmid  out: fence
    kNavi48SelWaitFence   = 6,   // in: fence, timeout_us  out: observed, elapsed_us
    kNavi48SelRegRead     = 7,   // in: absolute dword     out: value
    kNavi48SelSelfTest    = 8,   // in struct Navi48SelfTestIn, out struct …Out
    kNavi48SelGetCounters = 9,   // out struct Navi48Counters
    kNavi48SelReadLog     = 10,  // in: offset  out struct bytes; out scalar: total
    kNavi48SelMetrics     = 11,  // out struct Navi48Metrics (live PMFW telemetry)
    kNavi48SelPowerState  = 12,  // in: state (0 auto, 1 low, 2 nominal, 3 high, 4 peak)
    // in: action (0 = status only, 1 = fire)
    // out: installed, ttl_total_calls, ttl_first_unsupported_slot
    //
    // Fires the Phase 4 accelerator experiment ON DEMAND rather than at boot.
    // This is a safety property, not a convenience: if Apple's accelerator
    // panics while driving our TTL, an experiment that ran automatically at
    // boot would panic again on the next boot, and again, with nobody at the
    // keyboard to pick a different OpenCore entry. Fired from userspace, a
    // panic costs one reboot and the machine comes back up clean.
    kNavi48SelAccelExperiment = 13,
    //: drop the driver log so a measurement stands alone. n48_log_reset()
    // has existed since the log was written but was never reachable from
    // userspace, so a full buffer silently discarded every NEW line while
    // still reading back complete. That cost fifteen void captures.
    kNavi48SelLogReset        = 14,  // no args
    // 0.0.276: drop the FIRST `upTo` bytes of the driver log - bytes a reader has already copied out -
    // keeping everything appended since. `navi48test logstream` reads, appends to a file, F_FULLFSYNCs, then consumes,
    // so the ring never fills and a panic cannot take the log with it (rule 92).
    // in: upTo   out scalars: bytes still held, total consumed since boot, total bytes dropped while full, overflow flag
    kNavi48SelLogConsume      = 15,
    // 0.0.282: the binary CAPTURE ring (amd/n48cap.h), streamed exactly like the log. ReadCap: in offset, out struct
    // bytes (at most NAVI48_UC_MAX_XFER), out scalar held bytes. CapConsume: in upTo; out held, consumed total, dropped total,
    // records appended total.
    kNavi48SelReadCap         = 16,
    kNavi48SelCapConsume      = 17,
    // build 0.0.542 (apple/scanout_full.h): the capture `accel scanout 9` (`scanout full`) published - its 256-byte header then
    // the whole scanned-out surface. in: offset; out struct bytes (at most NAVI48_UC_MAX_XFER); out scalars: the capture's total
    // bytes (0 = none published), its number this boot. Read-only; `accel scanout 10` frees it. An older kext answers unsupported.
    kNavi48SelReadScanFull    = 18,
    kNavi48SelCount
};

// Largest payload a single Read/WriteVRAM call moves. Keeps the IOKit
// structure transfer inside the "small" path and bounds the kernel copy.
#define NAVI48_UC_MAX_XFER 4096u

// 0.0.263 — the driver log ring's capacity, in ONE place.
//
// It lived as two independent literals: kN48LogBytes in amd/n48log.h sized the ring, and
// navi48test.c re-hardcoded `512u*1024u - 1024u` to decide when to print "AT CAPACITY".
// Raising one without the other would have made the warning lie - and in the alarming
// direction, claiming a 2 MiB ring was full at 511 KiB. Both now derive from this.
//
// Raised 512 KiB -> 2 MiB because r96 AND r97 both hit the cap mid-run (~524,266 bytes) and
// dropped their late-boot lines, so neither run's log could be claimed complete - twice
// flagged, and the next question may live in exactly those dropped lines. A run is ~1300
// lines averaging well under 200 bytes; the instruments added in 0.0.261-0.0.263 (the
// unfiltered packet dump and its raw-dword lines) are what pushed it over. This is a static
// array in the kext, so the cost is 1.5 MiB of wired memory on a bring-up driver.
#define NAVI48_LOG_BYTES (2048u * 1024u)

struct Navi48Info {
    uint32_t abi_version;
    uint32_t stage_reached;      // BringupStage the ladder got to
    uint32_t stage_result;       // kern_return_t of the last stage
    uint32_t gc_version;         // (major << 16) | (minor << 8) | rev
    uint64_t vram_total_bytes;
    uint64_t vram_free_bytes;
    uint64_t vram_start_mc;      // MC address of VRAM byte 0
    uint32_t flags;              // see kNavi48Flag*
    uint32_t doorbell_gfx_ring0;
    uint32_t cp_ring_size_dwords;
    // Device-only pool above the BAR0 aperture (ABI 2). The CPU reaches it
    // only through the MM_INDEX window, so staging into it is slow; the GPU
    // sees no difference.
    uint64_t vram_hi_total_bytes;
    uint64_t vram_hi_free_bytes;
    uint32_t reserved[1];
};

// AllocVRAM flags (third scalar input; absent means 0).
#define kNavi48AllocDeviceOnly (1u << 0)   // take it from the pool above BAR0

#define kNavi48FlagCPReady      (1u << 0)   // GFX ring mapped and proven
#define kNavi48FlagMESReady     (1u << 1)
#define kNavi48FlagSDMAReady    (1u << 2)
#define kNavi48FlagIRQArmed     (1u << 3)
#define kNavi48FlagComputeOK    (1u << 4)   // stage 17 passed this boot

enum Navi48SelfTestID {
    kNavi48TestNopSubmit   = 0,  // N × (NOP + EOP fence + doorbell + wait)
    kNavi48TestWriteData   = 1,  // N × (WRITE_DATA magic to VRAM, verify)
    kNavi48TestFenceBurst  = 2,  // N fences in flight before waiting
    kNavi48TestCompute     = 3,  // N × the full stage-17 compute dispatch
    kNavi48TestIDCount
};

struct Navi48SelfTestIn {
    uint32_t test_id;
    uint32_t iterations;
    uint32_t timeout_us;         // per iteration
    uint32_t reserved;
};

struct Navi48SelfTestOut {
    uint32_t test_id;
    uint32_t iterations_run;
    uint32_t failures;
    uint32_t first_failure_iter;
    uint64_t elapsed_us;         // whole run, loop-step estimate
    uint64_t last_fence;
    uint32_t last_observed;      // test-specific witness value
    uint32_t kr;                 // first non-success kern_return_t
};

// A readable slice of PMFW's metrics table. Clocks in MHz, temperatures in
// degrees C, power in watts, activity in percent — the raw table's units.
struct Navi48Metrics {
    uint32_t gfxclk_mhz, socclk_mhz, uclk_mhz, fclk_mhz;
    uint32_t temp_edge_c, temp_hotspot_c, temp_mem_c;
    uint32_t socket_power_w, board_power_w;
    uint32_t gfx_activity_pct, mem_activity_pct;
    uint32_t fan_rpm, fan_pwm_pct;
    uint32_t pcie_gen, pcie_width;
    uint32_t metrics_counter;
    // The card's PMFW is newer than any published header (it reports interface
    // version 0x33; upstream pins 0x2E), so the decode above is best-effort.
    uint32_t if_version;        // what the firmware reported
    uint32_t layout_verified;   // 1 only if if_version matches the layout
    uint32_t values_plausible;  // 1 if the decoded values pass range checks
    uint32_t reserved[1];
};

struct Navi48Counters {
    uint64_t irq_count;
    uint64_t irq_entries;
    uint64_t irq_eop;
    uint64_t irq_faults;
    uint64_t irq_cp_errors;
    uint64_t irq_other;
    uint64_t submits;
    uint64_t fences_completed;
    uint64_t fence_timeouts;
};

#endif /* Navi48UserClientABI_h */
