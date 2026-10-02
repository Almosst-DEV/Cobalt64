//
//  navi48test — userspace harness for the Navi48Bringup kext.
//
//  Build (on the host Mac, for the PC):  tools/build-navi48test.sh
//  Run (on the PC, as root):        sudo ./navi48test <command> [args]
//
//  Commands:
//    info                       device + ladder state
//    counters                   interrupt / submission counters
//    reg <hexdword>             read one BAR5 register (absolute dword index)
//    regs                       read a LIST of BAR5 registers from stdin (T1 census), one
//                               connection, each line timestamped: REG <abs> <val> <us> <label>
//    poke                       alloc a page, write a pattern, read it back
//    submit                     one hand-built WRITE_DATA indirect buffer
//    log                        dump the driver's own log ring (independent of `log show`)
//    metrics [n]                live clocks / temperature / power, n times one second apart
//    power <0-4>                clamp the GFX clock: 0 auto, 1 low, 2 nominal, 3 high, 4 peak
//    bigmem [MiB]               allocate above the BAR0 window, stage + GPU-write it
//    test <id> <iters> [us]     in-kernel stress: 0 nop, 1 write-data, 2 fence-burst
//    suite [iters]              the reliability suite (default 10000 iterations)
//
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <IOKit/IOKitLib.h>
#include "../../src/navi48-bringup/src/Navi48UserClientABI.h"
#include "../../src/navi48-bringup/src/apple/gfx_dep.h"   /* 0.0.359: n48_ring_caught_up, the kext's own host-tested compare */
#include "../../src/navi48-bringup/src/apple/sdma_gcr.h"  /* 0.0.416: the mode-7 scalar packing (G2/G3), one source of truth */
#include "../../src/navi48-bringup/src/apple/sdma_dcc.h"  /* 0.0.417: the sdmadcc field decode, one source of truth */
#include "../../src/navi48-bringup/src/apple/scanout_copy.h" /* 0.0.417: N48_TILE_UNIFORM_PIXEL for the `scanout 8` report */
#include "../../src/navi48-bringup/src/apple/scanout_full.h" /* 0.0.542: `scanout full`'s header, reasons and checks */

static io_connect_t conn = IO_OBJECT_NULL;

static int open_service(void) {
    io_service_t svc = IOServiceGetMatchingService(
        kIOMainPortDefault, IOServiceMatching(NAVI48_UC_SERVICE_NAME));
    if (!svc) {
        fprintf(stderr, "navi48test: %s not found in the IORegistry — is the kext loaded "
                        "and did it match the GPU?\n", NAVI48_UC_SERVICE_NAME);
        return -1;
    }
    kern_return_t kr = IOServiceOpen(svc, mach_task_self(), 0, &conn);
    IOObjectRelease(svc);
    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "navi48test: IOServiceOpen failed (0x%x)%s\n", kr,
                kr == kIOReturnNotPermitted ? " — run with sudo" : "");
        return -1;
    }
    return 0;
}

static const char *stage_name(uint32_t s) {
    static const char *n[] = {"None","IPDiscovery","IHInit","GMCInit","PSPInit","PSPLoadSOS",
        "PSPRingCreate","TMRSetup","PSPFwLoad","SMUInit","IMUInit","RLCInit","CPInit",
        "MESInit","GFXInit","SDMAInit","PM4Test","ComputeDispatch"};
    return s < sizeof(n)/sizeof(n[0]) ? n[s] : "?";
}

static int cmd_info(void) {
    struct Navi48Info info; size_t sz = sizeof(info);
    kern_return_t kr = IOConnectCallStructMethod(conn, kNavi48SelGetInfo, NULL, 0, &info, &sz);
    if (kr != KERN_SUCCESS) { fprintf(stderr, "GetInfo failed 0x%x\n", kr); return 1; }
    printf("ABI version        %u\n", info.abi_version);
    printf("Stage reached      %u (%s), result 0x%x\n",
           info.stage_reached, stage_name(info.stage_reached), info.stage_result);
    printf("GC version         %u.%u.%u\n", info.gc_version >> 16,
           (info.gc_version >> 8) & 0xff, info.gc_version & 0xff);
    printf("VRAM               %llu MiB on the card, MC base 0x%llx\n",
           info.vram_total_bytes >> 20, info.vram_start_mc);
    printf("  CPU-visible pool %llu MiB free (through the BAR0 aperture)\n",
           info.vram_free_bytes >> 20);
    printf("  device-only pool %llu MiB free of %llu MiB (CPU reaches it via MM_INDEX)\n",
           info.vram_hi_free_bytes >> 20, info.vram_hi_total_bytes >> 20);
    printf("GFX ring           doorbell %u, %u dwords\n",
           info.doorbell_gfx_ring0, info.cp_ring_size_dwords);
    printf("Ready              CP:%s MES:%s SDMA:%s IRQ:%s compute:%s\n",
           (info.flags & kNavi48FlagCPReady)   ? "yes" : "no",
           (info.flags & kNavi48FlagMESReady)  ? "yes" : "no",
           (info.flags & kNavi48FlagSDMAReady) ? "yes" : "no",
           (info.flags & kNavi48FlagIRQArmed)  ? "armed" : "polled",
           (info.flags & kNavi48FlagComputeOK) ? "passed" : "not run");
    return 0;
}

static int cmd_counters(void) {
    struct Navi48Counters c; size_t sz = sizeof(c);
    kern_return_t kr = IOConnectCallStructMethod(conn, kNavi48SelGetCounters, NULL, 0, &c, &sz);
    if (kr != KERN_SUCCESS) { fprintf(stderr, "GetCounters failed 0x%x\n", kr); return 1; }
    printf("interrupts   %llu (%llu entries: eop %llu, faults %llu, cp-errors %llu, other %llu)\n",
           c.irq_count, c.irq_entries, c.irq_eop, c.irq_faults, c.irq_cp_errors, c.irq_other);
    printf("submissions  %llu, fences completed %llu, timeouts %llu\n",
           c.submits, c.fences_completed, c.fence_timeouts);
    return 0;
}

static int cmd_reg(const char *arg) {
    uint64_t in = strtoull(arg, NULL, 0), out = 0; uint32_t n = 1;
    kern_return_t kr = IOConnectCallScalarMethod(conn, kNavi48SelRegRead, &in, 1, &out, &n);
    if (kr != KERN_SUCCESS) { fprintf(stderr, "RegRead failed 0x%x\n", kr); return 1; }
    printf("reg[0x%llx] = 0x%08llx\n", in, out);
    return 0;
}

// `regs` — the T1 census reader (notes/DISPLAY-DESIGN.md section 6).
//
// Same selector as `reg`, but ONE process and ONE user-client connection for the
// whole list, read from stdin as `<abs_dword_hex> [label]` lines (# comments and
// blank lines skipped). Two reasons it exists rather than a shell loop over `reg`:
//
//   * 194 separate sudo+IOServiceOpen invocations cost tens of seconds of process
//     churn, which smears the 10 s window the frame-count arithmetic depends on;
//   * each line carries a CLOCK_MONOTONIC_RAW microsecond stamp taken immediately
//     BEFORE its read, so the refresh derived from two OTG_STATUS_FRAME_COUNT
//     samples uses that register's own elapsed time, not the script's sleep.
//
// READ ONLY: the only kernel call it can make is kNavi48SelRegRead.
// Output: `REG <abs> <value> <t_us> <label>` (or `ERR <kr>` in place of the value).
static int cmd_regs(void) {
    char line[512];
    unsigned long rows = 0, errs = 0;
    while (fgets(line, sizeof line, stdin)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\n' || *p == '\r' || *p == '\0') continue;
        char *end = NULL;
        uint64_t in = strtoull(p, &end, 0);
        if (end == p) continue;
        while (*end == ' ' || *end == '\t') end++;
        size_t L = strlen(end);
        while (L && (end[L-1] == '\n' || end[L-1] == '\r')) end[--L] = '\0';
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
        unsigned long long us = (unsigned long long)ts.tv_sec * 1000000ull
                              + (unsigned long long)(ts.tv_nsec / 1000);
        uint64_t out = 0; uint32_t n = 1;
        kern_return_t kr = IOConnectCallScalarMethod(conn, kNavi48SelRegRead, &in, 1, &out, &n);
        if (kr != KERN_SUCCESS) {
            printf("REG 0x%08llx ERR 0x%x %llu %s\n", in, kr, us, end);
            errs++;
        } else {
            printf("REG 0x%08llx 0x%08llx %llu %s\n", in, out, us, end);
        }
        rows++;
    }
    printf("REGS-DONE rows %lu errors %lu\n", rows, errs);
    fflush(stdout);
    return errs ? 1 : 0;
}

static int alloc_flags(uint64_t size, uint32_t flags, uint64_t *gpu_va, uint64_t *handle) {
    uint64_t in[3] = { size, 4096, flags }, out[2] = {0,0}; uint32_t n = 2;
    kern_return_t kr = IOConnectCallScalarMethod(conn, kNavi48SelAllocVRAM, in, 3, out, &n);
    if (kr != KERN_SUCCESS) { fprintf(stderr, "AllocVRAM failed 0x%x\n", kr); return -1; }
    *gpu_va = out[0]; *handle = out[1];
    return 0;
}
static int alloc_page(uint64_t size, uint64_t *gpu_va, uint64_t *handle) {
    return alloc_flags(size, 0, gpu_va, handle);
}
static void free_page(uint64_t handle) {
    IOConnectCallScalarMethod(conn, kNavi48SelFreeVRAM, &handle, 1, NULL, NULL);
}

static int cmd_poke(void) {
    uint64_t va = 0, h = 0;
    if (alloc_page(4096, &va, &h) < 0) return 1;
    printf("allocated 4096 bytes at MC 0x%llx (handle %llu)\n", va, h);

    uint32_t pattern[16];
    for (int i = 0; i < 16; i++) pattern[i] = 0x4E340000u + i;
    kern_return_t kr = IOConnectCallMethod(conn, kNavi48SelWriteVRAM, &va, 1,
                                           pattern, sizeof(pattern), NULL, NULL, NULL, NULL);
    if (kr != KERN_SUCCESS) { fprintf(stderr, "WriteVRAM failed 0x%x\n", kr); free_page(h); return 1; }

    uint32_t got[16]; size_t gsz = sizeof(got);
    uint64_t rin[2] = { va, sizeof(got) };
    kr = IOConnectCallMethod(conn, kNavi48SelReadVRAM, rin, 2, NULL, 0, NULL, NULL, got, &gsz);
    if (kr != KERN_SUCCESS) { fprintf(stderr, "ReadVRAM failed 0x%x\n", kr); free_page(h); return 1; }

    int bad = 0;
    for (int i = 0; i < 16; i++) if (got[i] != pattern[i]) bad++;
    printf("readback through the GPU's own view: %s (%d/16 dwords match)\n",
           bad ? "MISMATCH" : "ok", 16 - bad);
    if (bad) for (int i = 0; i < 4; i++) printf("  [%d] got %08x want %08x\n", i, got[i], pattern[i]);
    free_page(h);
    return bad ? 1 : 0;
}

// PM4 helpers mirroring the kernel's cp_pm4_gfx12.h.
static uint32_t p3(uint32_t op, uint32_t count_minus_1) {
    return (3u << 30) | ((count_minus_1 & 0x3FFFu) << 16) | ((op & 0xFFu) << 8);
}

static int cmd_submit(void) {
    uint64_t ib = 0, ibh = 0, tgt = 0, tgth = 0;
    if (alloc_page(4096, &ib, &ibh) < 0) return 1;
    if (alloc_page(4096, &tgt, &tgth) < 0) { free_page(ibh); return 1; }

    const uint32_t magic = 0xFEEDFACEu;
    uint32_t poison = 0xDEADBEEFu;
    IOConnectCallMethod(conn, kNavi48SelWriteVRAM, &tgt, 1, &poison, 4, NULL, NULL, NULL, NULL);

    uint32_t dw[5];
    dw[0] = p3(0x37, 3);                       // WRITE_DATA, 4 payload dwords
    dw[1] = (5u << 8) | (1u << 20);            // DST_SEL = memory, WR_CONFIRM
    dw[2] = (uint32_t)(tgt & 0xFFFFFFFFu);
    dw[3] = (uint32_t)(tgt >> 32);
    dw[4] = magic;
    kern_return_t kr = IOConnectCallMethod(conn, kNavi48SelWriteVRAM, &ib, 1,
                                           dw, sizeof(dw), NULL, NULL, NULL, NULL);
    if (kr != KERN_SUCCESS) { fprintf(stderr, "staging the IB failed 0x%x\n", kr); goto out; }

    uint64_t sin[3] = { ib, 5, 0 }, fence = 0; uint32_t n = 1;
    kr = IOConnectCallScalarMethod(conn, kNavi48SelSubmitIB, sin, 3, &fence, &n);
    if (kr != KERN_SUCCESS) { fprintf(stderr, "SubmitIB failed 0x%x\n", kr); goto out; }
    printf("submitted: fence %llu\n", fence);

    uint64_t win[2] = { fence, 1000000 }, wout[2] = {0,0}; n = 2;
    kr = IOConnectCallScalarMethod(conn, kNavi48SelWaitFence, win, 2, wout, &n);
    printf("fence %s after %llu us (slot holds %llu)\n",
           kr == KERN_SUCCESS ? "landed" : "TIMED OUT", wout[1], wout[0]);

    uint32_t got = 0; size_t gsz = sizeof(got);
    uint64_t rin[2] = { tgt, 4 };
    IOConnectCallMethod(conn, kNavi48SelReadVRAM, rin, 2, NULL, 0, NULL, NULL, &got, &gsz);
    printf("target dword = 0x%08x (want 0x%08x) => %s\n", got, magic,
           got == magic ? "PASSED — the CP executed a userspace-built packet" : "FAILED");
    kr = (got == magic) ? KERN_SUCCESS : 1;
out:
    free_page(tgth); free_page(ibh);
    return kr == KERN_SUCCESS ? 0 : 1;
}

static int run_test(uint32_t id, uint32_t iters, uint32_t timeout_us, int quiet) {
    struct Navi48SelfTestIn in = { id, iters, timeout_us, 0 };
    struct Navi48SelfTestOut out; size_t osz = sizeof(out);
    kern_return_t kr = IOConnectCallStructMethod(conn, kNavi48SelSelfTest,
                                                 &in, sizeof(in), &out, &osz);
    if (kr != KERN_SUCCESS) { fprintf(stderr, "SelfTest %u failed 0x%x\n", id, kr); return 1; }
    static const char *names[] = { "nop-submit", "write-data", "fence-burst", "compute" };
    printf("%-12s %7u/%-7u iterations, %u failures%s, %llu us\n",
           id < 4 ? names[id] : "?", out.iterations_run, iters, out.failures,
           out.failures ? "" : " — clean", out.elapsed_us);
    if (out.failures && !quiet)
        printf("             first failure at iteration %u, kr=0x%x, last witness 0x%08x\n",
               out.first_failure_iter, out.kr, out.last_observed);
    return out.failures ? 1 : 0;
}

// Allocate above the BAR0 aperture, stage a pattern through the MM_INDEX
// window, have the CP write into it, and read it back — the whole point being
// that none of this memory is reachable through BAR0.
static int cmd_bigmem(uint64_t mib) {
    uint64_t va = 0, h = 0;
    if (alloc_flags(mib << 20, kNavi48AllocDeviceOnly, &va, &h) < 0) return 1;
    printf("allocated %llu MiB of device-only VRAM at MC 0x%llx\n", mib, va);

    uint32_t pattern[8];
    for (int i = 0; i < 8; i++) pattern[i] = 0xB16B0000u + i;
    kern_return_t kr = IOConnectCallMethod(conn, kNavi48SelWriteVRAM, &va, 1,
                                           pattern, sizeof(pattern), NULL, NULL, NULL, NULL);
    if (kr != KERN_SUCCESS) { fprintf(stderr, "WriteVRAM failed 0x%x\n", kr); free_page(h); return 1; }

    uint32_t got[8]; size_t gsz = sizeof(got);
    uint64_t rin[2] = { va, sizeof(got) };
    IOConnectCallMethod(conn, kNavi48SelReadVRAM, rin, 2, NULL, 0, NULL, NULL, got, &gsz);
    int bad = 0;
    for (int i = 0; i < 8; i++) if (got[i] != pattern[i]) bad++;
    printf("CPU staged + read back through MM_INDEX: %s (%d/8)\n", bad ? "MISMATCH" : "ok", 8 - bad);

    // Now make the GPU write there, from an IB that lives in the low pool.
    uint64_t ib = 0, ibh = 0;
    if (alloc_page(4096, &ib, &ibh) == 0) {
        const uint32_t magic = 0x0DDBA11Du;
        uint32_t dw[5] = { p3(0x37, 3), (5u << 8) | (1u << 20),
                           (uint32_t)(va & 0xFFFFFFFFu), (uint32_t)(va >> 32), magic };
        IOConnectCallMethod(conn, kNavi48SelWriteVRAM, &ib, 1, dw, sizeof(dw), NULL, NULL, NULL, NULL);
        uint64_t sin[3] = { ib, 5, 0 }, fence = 0; uint32_t n = 1;
        if (IOConnectCallScalarMethod(conn, kNavi48SelSubmitIB, sin, 3, &fence, &n) == KERN_SUCCESS) {
            uint64_t win[2] = { fence, 1000000 }, wout[2] = {0,0}; n = 2;
            IOConnectCallScalarMethod(conn, kNavi48SelWaitFence, win, 2, wout, &n);
            uint32_t g = 0; size_t s2 = sizeof(g); uint64_t r2[2] = { va, 4 };
            IOConnectCallMethod(conn, kNavi48SelReadVRAM, r2, 2, NULL, 0, NULL, NULL, &g, &s2);
            printf("GPU wrote 0x%08x (want 0x%08x) => %s\n", g, magic,
                   g == magic ? "PASSED — the command processor reached memory outside the BAR0 window"
                              : "FAILED");
            if (g != magic) bad++;
        }
        free_page(ibh);
    }
    free_page(h);
    return bad ? 1 : 0;
}

// Pull the driver's own log ring. This is the only reliable way to read what
// the bring-up code printed: since the kext moved into the boot collection,
// `log show` no longer carries its lines at all.
static int cmd_log(void) {
    uint64_t offset = 0, total = 0;
    char chunk[NAVI48_UC_MAX_XFER + 1];
    for (;;) {
        size_t sz = NAVI48_UC_MAX_XFER;
        uint32_t n = 1;
        kern_return_t kr = IOConnectCallMethod(conn, kNavi48SelReadLog, &offset, 1, NULL, 0,
                                               &total, &n, chunk, &sz);
        if (kr != KERN_SUCCESS) { fprintf(stderr, "ReadLog failed 0x%x\n", kr); return 1; }
        if (sz == 0) break;
        chunk[sz] = 0;
        fputs(chunk, stdout);
        offset += sz;
        if (offset >= total) break;
    }
    fprintf(stderr, "\n(%llu bytes of driver log%s)\n", total,
            // 0.0.263: from the SHARED NAVI48_LOG_BYTES, never a second literal. This
            // line used to re-hardcode 512 KiB independently of the ring it describes.
            total >= NAVI48_LOG_BYTES - 1024u ? "  *** AT CAPACITY - NEW LINES ARE BEING DROPPED, run `logreset` ***" : "");
    return 0;
}

// 0.0.276 : stream the driver log to disk so a watchdog panic cannot lose it (rule 92).
// Every `interval_ms`: read everything the ring holds, append it to `path`, F_FULLFSYNC the file, sync(2) the
// system, then ask the kext to CONSUME exactly the bytes written - lines appended meanwhile stay for the next pass.
// Stops when `stopfile` exists or after `max_s`. Opens only our own user client, never the accelerator.
static int cmd_logstream(const char *path, int interval_ms, int max_s, const char *stopfile) {
    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) { perror("logstream open"); return 1; }
    if (interval_ms < 100) interval_ms = 100;
    if (max_s <= 0) max_s = 3600;
    char hdr[256];
    int hn = snprintf(hdr, sizeof hdr, "### logstream start pid %d interval %d ms max %d s stopfile %s\n",
                      (int)getpid(), interval_ms, max_s, stopfile ? stopfile : "-");
    if (write(fd, hdr, (size_t)hn) < 0) { perror("logstream write"); }
    char *buf = (char *)malloc(NAVI48_LOG_BYTES + 1);
    if (!buf) { close(fd); return 1; }
    time_t t0 = time(NULL);
    uint64_t passes = 0, bytes = 0, consumed = 0, dropped = 0, lastDropped = 0;
    for (;;) {
        uint64_t offset = 0, total = 0;
        size_t got = 0;
        for (;;) {
            size_t sz = NAVI48_UC_MAX_XFER;
            uint32_t n = 1;
            if (got + sz > NAVI48_LOG_BYTES) sz = NAVI48_LOG_BYTES - got;
            if (sz == 0) break;
            kern_return_t kr = IOConnectCallMethod(conn, kNavi48SelReadLog, &offset, 1, NULL, 0, &total, &n, buf + got, &sz);
            if (kr != KERN_SUCCESS) { fprintf(stderr, "logstream: ReadLog failed 0x%x\n", kr); sz = 0; }
            if (sz == 0) break;
            got += sz; offset += sz;
            if (offset >= total) break;
        }
        if (got) {
            size_t done = 0;
            while (done < got) {
                ssize_t w = write(fd, buf + done, got - done);
                if (w <= 0) { perror("logstream write"); break; }
                done += (size_t)w;
            }
            (void)fcntl(fd, F_FULLFSYNC);
            sync();
            uint64_t in = done, out[4] = {0};
            uint32_t on = 4;
            kern_return_t kr = IOConnectCallScalarMethod(conn, kNavi48SelLogConsume, &in, 1, out, &on);
            if (kr != KERN_SUCCESS) {
                fprintf(stderr, "logstream: LogConsume failed 0x%x - the kext predates 0.0.276; stopping\n", kr);
                break;
            }
            bytes += done; consumed = out[1]; dropped = out[2];
            if (dropped != lastDropped) {
                int n2 = snprintf(hdr, sizeof hdr, "### logstream: %llu byte(s) DROPPED by a full ring so far\n",
                                  (unsigned long long)dropped);
                if (write(fd, hdr, (size_t)n2) < 0) { perror("logstream write"); }
                (void)fcntl(fd, F_FULLFSYNC);
                lastDropped = dropped;
            }
        }
        passes++;
        if (stopfile && access(stopfile, F_OK) == 0) break;
        if (time(NULL) - t0 >= max_s) break;
        usleep((useconds_t)interval_ms * 1000u);
    }
    int n3 = snprintf(hdr, sizeof hdr, "### logstream end: %llu pass(es), %llu byte(s) written, kext consumed %llu, dropped %llu\n",
                      (unsigned long long)passes, (unsigned long long)bytes, (unsigned long long)consumed,
                      (unsigned long long)dropped);
    if (write(fd, hdr, (size_t)n3) < 0) { perror("logstream write"); }
    (void)fcntl(fd, F_FULLFSYNC);
    close(fd);
    free(buf);
    printf("logstream: %llu pass(es), %llu byte(s) written to %s, dropped %llu\n", (unsigned long long)passes,
           (unsigned long long)bytes, path, (unsigned long long)dropped);
    return 0;
}

// 0.0.282 : stream the kext's BINARY capture ring to a file, exactly as logstream streams the log: read everything
// held, append, F_FULLFSYNC, then consume what was written. Records are appended whole in the kext, so each pass ends on a record
// boundary. A text trailer goes to stdout, never into the binary file.
static int cmd_capstream(const char *path, int interval_ms, int max_s, const char *stopfile) {
    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) { perror("capstream open"); return 1; }
    if (interval_ms < 100) interval_ms = 100;
    if (max_s <= 0) max_s = 3600;
    printf("capstream: start pid %d interval %d ms max %d s stopfile %s -> %s\n", (int)getpid(), interval_ms, max_s,
           stopfile ? stopfile : "-", path);
    fflush(stdout);
    const size_t cap = 64u * 1024u * 1024u;
    char *buf = (char *)malloc(cap);
    if (!buf) { close(fd); return 1; }
    time_t t0 = time(NULL);
    uint64_t passes = 0, bytes = 0, consumed = 0, dropped = 0, records = 0, lastDropped = 0;
    int fails = 0;
    for (;;) {
        uint64_t offset = 0, total = 0;
        size_t got = 0;
        for (;;) {
            size_t sz = NAVI48_UC_MAX_XFER;
            uint32_t n = 1;
            if (got + sz > cap) sz = cap - got;
            if (sz == 0) break;
            kern_return_t kr = IOConnectCallMethod(conn, kNavi48SelReadCap, &offset, 1, NULL, 0, &total, &n, buf + got, &sz);
            if (kr != KERN_SUCCESS) { fprintf(stderr, "capstream: ReadCap failed 0x%x\n", kr); sz = 0; fails++; }
            if (sz == 0) break;
            got += sz; offset += sz;
            if (offset >= total) break;
        }
        if (got) {
            size_t done = 0;
            while (done < got) {
                ssize_t w = write(fd, buf + done, got - done);
                if (w <= 0) { perror("capstream write"); break; }
                done += (size_t)w;
            }
            (void)fcntl(fd, F_FULLFSYNC);
            uint64_t in = done, out[4] = {0};
            uint32_t on = 4;
            kern_return_t kr = IOConnectCallScalarMethod(conn, kNavi48SelCapConsume, &in, 1, out, &on);
            if (kr != KERN_SUCCESS) { fprintf(stderr, "capstream: CapConsume failed 0x%x - kext predates 0.0.282; stopping\n", kr); break; }
            bytes += done; consumed = out[1]; dropped = out[2]; records = out[3];
            if (dropped != lastDropped) {
                printf("capstream: %llu byte(s) DROPPED by a full capture ring so far\n", (unsigned long long)dropped);
                fflush(stdout);
                lastDropped = dropped;
            }
        }
        passes++;
        if (fails > 20) break;
        if (stopfile && access(stopfile, F_OK) == 0) break;
        if (time(NULL) - t0 >= max_s) break;
        usleep((useconds_t)interval_ms * 1000u);
    }
    close(fd);
    free(buf);
    printf("capstream end: %llu pass(es), %llu byte(s) written to %s, kext consumed %llu, dropped %llu, records %llu\n",
           (unsigned long long)passes, (unsigned long long)bytes, path, (unsigned long long)consumed, (unsigned long long)dropped,
           (unsigned long long)records);
    return 0;
}

// Live telemetry. Repeats if asked, so you can watch clocks and temperature
// move while something else drives the card.
static int cmd_metrics(int repeat) {
    for (int i = 0; i < (repeat > 0 ? repeat : 1); i++) {
        struct Navi48Metrics m; size_t sz = sizeof(m);
        kern_return_t kr = IOConnectCallStructMethod(conn, kNavi48SelMetrics, NULL, 0, &m, &sz);
        if (kr != KERN_SUCCESS) { fprintf(stderr, "Metrics failed 0x%x\n", kr); return 1; }
        if (!m.layout_verified)
            printf("!! firmware driver-interface version 0x%x is newer than the layout this\n"
                   "!! build decodes; values are UNVERIFIED (range check: %s)\n",
                   m.if_version, m.values_plausible ? "passed" : "FAILED");
        printf("clocks  gfx %4u MHz  soc %4u MHz  mem %4u MHz  fclk %4u MHz\n",
               m.gfxclk_mhz, m.socclk_mhz, m.uclk_mhz, m.fclk_mhz);
        printf("temps   edge %3u C  hotspot %3u C  mem %3u C   fan %u rpm (%u%%)\n",
               m.temp_edge_c, m.temp_hotspot_c, m.temp_mem_c, m.fan_rpm, m.fan_pwm_pct);
        printf("power   socket %3u W  board %3u W   busy gfx %3u%%  mem %3u%%\n",
               m.socket_power_w, m.board_power_w, m.gfx_activity_pct, m.mem_activity_pct);
        printf("link    PCIe gen %u x%u   (snapshot %u)\n\n",
               m.pcie_gen, m.pcie_width, m.metrics_counter);
        if (i + 1 < repeat) sleep(1);
    }
    return 0;
}

static int cmd_suite(uint32_t iters) {
    printf("=== Navi48 reliability suite (%u iterations per test) ===\n", iters);
    int bad = 0;
    bad |= cmd_poke();
    bad |= cmd_submit();
    bad |= cmd_bigmem(1024);
    for (uint32_t id = 0; id < kNavi48TestIDCount; id++) {
        // The compute dispatch reallocates and re-verifies every lane, so it
        // runs at roughly a millisecond each — cap it rather than the others.
        uint32_t n = (id == kNavi48TestCompute) ? (iters > 200 ? 200 : iters) : iters;
        bad |= run_test(id, n, 2000000, 0);
    }
    cmd_counters();
    printf("=== suite %s ===\n", bad ? "FAILED" : "PASSED");
    return bad;
}


// accel [status|fire|memenable|synctables|enablerings|startengines|ringstate|dumpring|programqueue|ringhooks|dumpib|rebaseib|enablequeue|kickdoorbell|ringrefs|queuestate|opengate|neuterpoll|chanstate|pokecompletion|signalcompletion|bindchannel|schedstate|stampstate|signalstamp|stampgap|runcheckts|runadvance|xlatregs|srbmprobe|resume|pm4powerup|setvspace|kiqenable|kiqstamp|kiqchan|gfxmap|gfxstate|sdmamap|sdmastate|faultclear|vmstate|vmib [va]|ringib [chan]|pagecopy [1]|flushdrop|kernsub [1-4]|vmpage [va]|renderxlat [mode]|eopbridge [1|2]|bootchain [mode]|shadercache [1|2|3]|vmroots [addr]|ringmap [0|1]|vmctx|rootwrite [0|1]|rearmdrain [0|1]|pairing [1|2]|drain] — the Phase 4 experiment.
//
// "fire" installs the TTL hook and lets Apple's AMDRadeonX6000 accelerator match
// this card, driving the GPU through our Navi48Ttl instead of Apple's. It is a
// separate command rather than something the driver does at boot on purpose: if
// Apple's accelerator panics, a boot-time trigger would panic again on every
// subsequent boot with nobody at the OpenCore picker. This way a panic costs one
// reboot and the machine comes back clean.
//
// Run `log` afterwards: the TTL call trace is the actual result.
// build 0.0.542 (apple/scanout_full.h): pull the capture `accel scanout full` published (kNavi48SelReadScanFull, 4 KiB per call),
// check it (the header's own consistency, the byte count, the payload's FNV-1a, the capture number unchanged under the read), write
// header + payload to `path` (never over an existing file), then free it in the kext (`scanout 10`). 0 = written.
static int scanfull_pull(const char *path, uint64_t wantTotal, uint64_t wantSeq) {
    char defpath[128];
    if (!path) {
        time_t now = time(NULL);
        struct tm tmv;
        localtime_r(&now, &tmv);
        char ts[32];
        strftime(ts, sizeof ts, "%Y%m%d-%H%M%S", &tmv);
        snprintf(defpath, sizeof defpath, "scanout-full-%llu-%s.n48scan", (unsigned long long)wantSeq, ts);
        path = defpath;
    }
    if (wantTotal < N48_SF_HDR_BYTES || wantTotal > N48_SF_HDR_BYTES + N48_SF_MAX_PAYLOAD) {
        printf("  scanout full: the kext reports %llu byte(s) - REFUSING to read\n", (unsigned long long)wantTotal);
        return 3;
    }
    uint8_t *buf = (uint8_t *)malloc((size_t)wantTotal);
    if (!buf) return 3;
    uint64_t off = 0, calls = 0;
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    while (off < wantTotal) {
        // build 0.0.543 item E4 (the 0.0.542 review's SHOULD-FIX): never offer the kext more room than this buffer has left - a
        // larger capture published mid-pull could otherwise write past `buf` before the total/seq check below refuses it.
        const uint64_t left = wantTotal - off;
        size_t sz = left < NAVI48_UC_MAX_XFER ? (size_t)left : (size_t)NAVI48_UC_MAX_XFER;
        uint64_t so[2] = { 0, 0 };
        uint32_t n = 2;
        kern_return_t kr = IOConnectCallMethod(conn, kNavi48SelReadScanFull, &off, 1, NULL, 0, so, &n, buf + off, &sz);
        calls++;
        if (kr != KERN_SUCCESS) { printf("  scanout full: ReadScanFull at %llu failed 0x%x%s\n", (unsigned long long)off, kr,
                                         kr == kIOReturnUnsupported ? " (a kext older than 0.0.542)" : ""); free(buf); return 3; }
        if (so[0] != wantTotal || so[1] != wantSeq) {
            printf("  scanout full: the capture CHANGED under the read (total %llu seq %llu, wanted %llu / %llu) - nothing written\n",
                   (unsigned long long)so[0], (unsigned long long)so[1], (unsigned long long)wantTotal, (unsigned long long)wantSeq);
            free(buf); return 3;
        }
        if (sz == 0 || sz > wantTotal - off) { printf("  scanout full: a short read at %llu - nothing written\n", (unsigned long long)off); free(buf); return 3; }
        off += sz;
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    n48_sf_hdr h;
    memcpy(&h, buf, sizeof h);
    const uint32_t hc = n48_sf_hdr_check(&h, wantTotal);
    const uint32_t fnv = n48_sf_fnv32(buf + N48_SF_HDR_BYTES, wantTotal - N48_SF_HDR_BYTES);
    if (hc || fnv != h.fnv32) {
        printf("  scanout full: the capture does NOT check (header check %u, FNV-1a %08x vs the header's %08x) - nothing written\n",
               hc, fnv, h.fnv32);
        free(buf); return 3;
    }
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (fd < 0) { perror("  scanout full: open (an existing file is never overwritten)"); free(buf); return 3; }
    size_t done = 0;
    while (done < wantTotal) {
        ssize_t w = write(fd, buf + done, (size_t)wantTotal - done);
        if (w <= 0) { perror("  scanout full: write"); break; }
        done += (size_t)w;
    }
    (void)fcntl(fd, F_FULLFSYNC);
    close(fd);
    free(buf);
    if (done != wantTotal) return 3;
    const double ms = (double)(t1.tv_sec - t0.tv_sec) * 1e3 + (double)(t1.tv_nsec - t0.tv_nsec) / 1e6;
    time_t ct = (time_t)h.cal_sec;
    struct tm tmv;
    localtime_r(&ct, &tmv);
    char ts[40];
    strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", &tmv);
    printf("  scanout full WRITTEN    : %s (%llu bytes = 256-byte header + %llu payload; %llu reads in %.1f ms)\n", path,
           (unsigned long long)wantTotal, (unsigned long long)h.payload_bytes, (unsigned long long)calls, ms);
    printf("  surface                 : %s MC 0x%llx (vram+0x%llx) %ux%u pitch %u fmt %u SW_MODE %u; flags 0x%x; FNV-1a %08x\n",
           h.which == 1 ? "A (console)" : h.which == 2 ? "B (flip mode)" : "?", (unsigned long long)h.surface_mc,
           (unsigned long long)h.surface_vram_off, h.width, h.height, h.pitch_bytes, h.dcn_format, h.sw_mode, h.flags, h.fnv32);
    printf("  captured at             : %s.%06u local (uptime %llu us), copy %u us in %u chunk(s), OTG0 frames %u -> %u, capture #%llu\n",
           ts, h.cal_usec, (unsigned long long)h.uptime_us, h.copy_us, h.chunks, h.fc_before, h.fc_after, (unsigned long long)h.seq);
    // build 0.0.543 item E5 (header version 2): the viewport start and the fast copy's state as the capture left it.
    printf("  viewport start / copy   : (%u, %u); writes during %u; 63 %s (why %u); 89 buffer %s%s%s, mode %u\n", h.vp_x, h.vp_y,
           h.writers_during, h.fc63_latched ? "LATCHED OFF" : "live", h.fc63_why, (h.fc89_state & 1u) ? "bound" : "not bound",
           (h.fc89_state & 2u) ? " RETIRED" : "", (h.fc89_state & 4u) ? " (control run)" : "", h.fc89_mode);
    uint64_t in2[2] = { 60, N48_SF_MODE_RELEASE }, out[16] = { 0 };
    uint32_t outCnt = 16;
    kern_return_t kr = IOConnectCallScalarMethod(conn, kNavi48SelAccelExperiment, in2, 2, out, &outCnt);
    printf("  kext capture released   : %s\n", kr == KERN_SUCCESS && out[3] == 0 ? "yes (`scanout 10`)" : "NO - send `accel scanout 10`");
    return 0;
}

static int cmd_accel(const char *what, const char *arg, const char *arg2, const char *arg3) {
    // 0 = status, 1 = fire, 2 = re-drive AMDHardware::setMemoryAllocationsEnabled(true)
    // so the 68 MiB page-table VRAM allocation it makes can be traced; see
    // Navi48Bringup::accelExperiment for why that has to be callable on demand.
    uint64_t in = 0;
    if (what && !strcmp(what, "fire"))           in = 1;
    else if (what && !strcmp(what, "memenable")) in = 2;
    else if (what && !strcmp(what, "synctables")) in = 3;
    else if (what && !strcmp(what, "enablerings")) in = 4;
    else if (what && !strcmp(what, "startengines")) in = 5;
    else if (what && !strcmp(what, "ringstate")) in = 6;
    else if (what && !strcmp(what, "dumpring")) in = 7;
    else if (what && !strcmp(what, "programqueue")) in = 8;
    else if (what && !strcmp(what, "ringhooks")) in = 9;
    else if (what && !strcmp(what, "dumpib")) in = 10;
    else if (what && !strcmp(what, "rebaseib")) in = 11;
    else if (what && !strcmp(what, "enablequeue")) in = 12;
    else if (what && !strcmp(what, "kickdoorbell")) in = 13;
    else if (what && !strcmp(what, "ringrefs")) in = 14;
    else if (what && !strcmp(what, "queuestate")) in = 15;
    else if (what && !strcmp(what, "opengate")) in = 16;
    else if (what && !strcmp(what, "neuterpoll")) in = 17;
    else if (what && !strcmp(what, "chanstate")) in = 18;
    else if (what && !strcmp(what, "pokecompletion")) in = 19;
    else if (what && !strcmp(what, "signalcompletion")) in = 20;
    else if (what && !strcmp(what, "bindchannel")) in = 21;
    else if (what && !strcmp(what, "schedstate")) in = 22;
    else if (what && !strcmp(what, "stampstate")) in = 23;
    else if (what && !strcmp(what, "signalstamp")) in = 24;
    else if (what && !strcmp(what, "stampgap")) in = 25;
    else if (what && !strcmp(what, "runcheckts")) in = 26;
    else if (what && !strcmp(what, "runadvance")) in = 27;
    else if (what && !strcmp(what, "xlatregs")) in = 28;
    else if (what && !strcmp(what, "srbmprobe")) in = 29;
    else if (what && !strcmp(what, "resume")) in = 30;
    else if (what && !strcmp(what, "pm4powerup")) in = 31;
    else if (what && !strcmp(what, "setvspace")) in = 32;
    else if (what && !strcmp(what, "kiqenable")) in = 33;
    // 34/35 — route 1. `kiqstamp` arms a kernel-thread poller that, on every
    // submission PAST THE BASELINE it took at arm time, writes the WRITE-BACK
    // dword at *[chan+0xc0] — emulating the GPU, not Apple — and then watches
    // whether Apple's own checkForTimestampUpdate copies it into chan+0x84,
    // falling back to a direct +0x84 write (and saying so) after 300 ms.
    // `kiqchan` reads the outcome afterwards, because pm4powerup blocks for 5 s
    // in between, and also dumps the KIQ ring so the MAP_QUEUES frame is visible.
    else if (what && !strcmp(what, "kiqstamp")) in = 34;
    else if (what && !strcmp(what, "kiqchan")) in = 35;
    // 36/37 — 0.0.178, route 2's first increment. `gfxmap` SUPERSEDES kiqstamp
    // for a boot: it decodes the 32-dword KIQ frames instead of only counting
    // them, and on MAP_QUEUES(engine_sel 4) it hands GFX pipe0/queue0 from our
    // kernel GFX queue to APPLE'S ring through MES REMOVE_QUEUE + ADD_QUEUE, so
    // doStart's gate at 0xbe24357 reads the ring Apple is actually asking about.
    // Until that is verified, a live-zero CP_RB0_RPTR is answered with 1 so the
    // gate fails and doStart unwinds the bounded way. `gfxstate` reads it back.
    else if (what && !strcmp(what, "gfxmap")) in = 36;
    else if (what && !strcmp(what, "gfxstate")) in = 37;
    // 38/39 — 0.0.185, the SDMA counterpart of gfxmap (design memo
    // notes/re/sdma-takeover-design.md, recorded as an earlier analysis). `sdmamap`
    // puts APPLE'S SDMA ring (chan id 14, the (queue 10, inst 1) ring at
    // 0x8400220000) onto OUR SDMA0 QUEUE1 at doorbell dword index 0x202 by direct
    // register programming - no MES, no MQD - after PTE-checking every page of
    // the ring and of its write-back frame, and then rewrites Apple's
    // ring->0xc0 to the real BAR2 doorbell so AMDRTRing::writeTail rings
    // something the hardware is listening on. Our own SDMA0 QUEUE0 is left
    // running as the control. `sdmastate` reads it all back, decodes the ring,
    // and writes nothing.
    else if (what && !strcmp(what, "sdmamap")) in = 38;
    else if (what && !strcmp(what, "sdmastate")) in = 39;
    // 40/41 — 0.0.193, the fourth Apple-vs-gfx12 encoding wall .
    // GCVM_L2_PROTECTION_FAULT_STATUS is FIRST-FAULT LATCHED, so every status word
    // read so far belonged to whichever fault happened first since boot.
    // `faultclear` pulses GCVM_L2_PROTECTION_FAULT_CNTL bit 0 (and prints the word
    // it discards) so the next read belongs to the next dispatch — run it
    // IMMEDIATELY BEFORE the measurement. `vmstate` reads, and only reads, Apple's
    // GCVM_CONTEXT2 registers, the latched status decoded field by field, and four
    // entries of Apple's own page-table arena through the MM_INDEX window.
    else if (what && !strcmp(what, "faultclear")) in = 40;
    else if (what && !strcmp(what, "vmstate")) in = 41;
    // 42 — 0.0.194, `vmib [va]`. found Apple's blit command buffer: an
    // INDIRECT_BUFFER at VA 0x4000a0000 under VMID 2, 0x80 dwords, which the CP
    // fetched without faulting and then parked inside. `vmib` software-walks
    // Apple's VMID-2 table for that VA (or one given on the command line),
    // prints root/L1/sub-table decoded, and dumps + PM4-decodes the page it
    // lands on. Read-only; it writes nothing anywhere.
    else if (what && !strcmp(what, "vmib")) in = 42;
    // 43 — 0.0.196, `ringib [chan]`. found Apple's blit kernel page EMPTY
    // because the shader upload sits on an SDMA ring nothing fetches: chan 13
    // holds three real COND_EXE-gated frames that no engine has ever read.
    // `ringib` decodes ONE channel's SDMA ring packet by packet AND dumps +
    // decodes every INDIRECT_BUFFER it points at (through the GART, as `dumpib`
    // does), so a COPY_LINEAR inside those frames would be visible by name. It
    // also prints the (queue type, inst) the channel maps to, from the TTL's own
    // slot-36 record. Read-only; it writes nothing anywhere. Default chan 14.
    else if (what && !strcmp(what, "ringib")) in = 43;
    // 44 — 0.0.201, `pagecopy [1]`. With 1 it ARMS the residency copy for this boot:
    // the skip-pagecopy hook then copies Apple's sysmem -> AMDAccelVidMemory page-on
    // into VRAM through the MM window and reads every dword back. Without it, it only
    // prints the counters (copies, read-back mismatches, unhandled shapes, page-outs).
    else if (what && !strcmp(what, "pagecopy")) in = 44;
    // 45 — 0.0.202, `flushdrop`. AN INSTRUMENT : after xlatregs and
    // before sdmamap, rewrite the EVENT_WRITE CS_PARTIAL_FLUSH that follows the compute
    // DISPATCH_DIRECT in Apple's VMID-2 blit IB into two one-dword NOPs, so the ME can
    // finish the IB while the waves stay stuck (section 317).
    else if (what && !strcmp(what, "flushdrop")) in = 45;
    // 46 — 0.0.204, `kernsub`. Substitute a gfx1201 blit kernel for Apple's Navi21
    // one at the residency copy's shader region (VRAM lastDst + 0xfb00), guarded by
    // an exact match on Apple's original bytes . Run it AFTER the
    // residency copy (after blit2start) and BEFORE the CP dispatches. `kernsub 1`
    // arms + substitutes; without the arg it only reads the counters.
    // 0.0.206: `kernsub 2` = blit_diag_gfx1201, `kernsub 3` = blit_diagmin_gfx1201
    // (INSTRUMENTS: id registers into blit2's destination page, nothing copied,
    // an earlier analysis); `kernsub 1` is still the copy kernel. One mode per boot.
    else if (what && !strcmp(what, "kernsub")) in = 46;
    // 47 — 0.0.206, `vmpage [va]`. Read-only: vmib's walk, then the WHOLE 4 KiB page
    // (default VA 0x400004000, blit2's destination) with the blit_diag records decoded
    // and a planted-buffer self-test of the decoder. Every result is an out-scalar.
    else if (what && !strcmp(what, "vmpage")) in = 47;
    // 48 — 0.0.209, `renderxlat [mode]` . After xlatregs, BEFORE
    // sdmamap: find Apple's render IB by content among the pending page-table leaves.
    // 0 census + dump (read-only), 3 BLANK (instrument), 1 translate in place; 0x100
    // seeds NGG; 2 is refused (0.0.208's VMID-0 repoint, retired).
    else if (what && !strcmp(what, "renderxlat")) in = 48;
    // 49 — 0.0.235, `eopbridge [1|2]` (milestone 3 step 1, an earlier analysis).
    // On an end-of-pipe IH entry our handler calls Apple's OWN
    // AMDSWScheduler::checkTimestamps, so a command buffer retires without a
    // blocked client and without the forced `runcheckts`. 1 arms for the boot,
    // 2 disarms, no argument reads the counters. Boot-arg navi48-eop-bridge=1
    // arms it at start(). Needs `fire` (it resolves Apple's scheduler).
    else if (what && !strcmp(what, "eopbridge")) in = 49;
    // 50 — 0.0.237, `bootchain` (milestone 3 step 3, an earlier analysis). Read-only:
    // what the in-kext boot chain did this boot. The chain is armed by the boot-arg
    // navi48-boot-chain (bitmask: 1 the accelerator-start sequence, 2 the
    // submission-time takeover). `bootchain <mode>` sets that mode as a TEST lever
    // and must be sent BEFORE `fire`, because the chain arms from Apple's
    // accelerator-started callback and `fire` is what triggers it; with no argument
    // the verb is read-only.
    else if (what && !strcmp(what, "bootchain")) in = 50;
    // 57 — 0.0.267, `pairing [1|2]` . The display-pairing stamp is OPT-IN:
    // no argument reads; 1 enables it for this boot and must be sent BEFORE `fire`; 2 disables
    // it or withdraws the keys we stamped. Registry edits only. Exits 3 on a refusal.
    else if (what && !strcmp(what, "pairing")) in = 57;
    // 58 — 0.0.269, `drain` . READ-ONLY: the state and counters of the drain, which boot-chain
    // bit 2 arms at the end of phase A and which translates every Apple SDMA submission before its doorbell.
    else if (what && !strcmp(what, "drain")) in = 58;
    // 59 — 0.0.272, `flushhook [1|2|3]` . Route c' wall 3: the per-surface externalMethod hook,
    // installed on every AMDAccelSurface the accelerator mints after arming. 1 log-only, 3 log + copy of a flushed
    // surface into RDNA4FB's scanout (refused unless `scanout 1` passed this boot), 2 pass-through, no argument reads.
    else if (what && !strcmp(what, "flushhook")) in = 59;
    // 60 — 0.0.272, `scanout [0|1|2|3|4|5|6|7 <vramOff> [gcr]|8|full [file]|10]` (an earlier analysis; 6 is 0.0.414,
    // build 0.0.542: `scanout full [file]` (mode 9) copies the WHOLE surface the display engine is scanning out (HUBP0's
    // SURFACE_EARLIEST_INUSE: the console A or flip mode's B) into the kext by SDMA, then this tool pulls it and writes a 256-byte
    // header + the raw surface to [file] (default ./scanout-full-<n>-<time>.n48scan; never over an existing file) and frees it
    // (`scanout 10`). READ-ONLY toward the display; decode with tools/runkit/scanout2png.py.
    // notes/design/SCANOUT-SELFTEST-FULL.md; 7 is 0.0.416, notes/design/SDMA-GCR.md; 8 is 0.0.417,
    // notes/design/SDMA-DCC-NOPTE.md D7 - the 1920x1080 UNIFORM-probe copy of mode 6's live case).
    // 6 (0.0.414, section 950) is the FULL-GEOMETRY SDMA self-test: one call, the 256x256 section 719 control then
    // the live 1920x1080 ADDR3 64KB_2D geometry, on our own three scratch VRAM buffers. 7 (0.0.416, section 953) is
    // the SDMA CACHE-RINSE instrument: `scanout 7 <vramOff>` copies a 256 KiB plane window linearly into our scratch
    // and compares the scratch with the source through the MM window; the optional `gcr` token puts the SDMA GCR_REQ
    // (GL2 write-back + invalidate) immediately before the copy in the same submission; `scanout 7 0` is the CONTROL
    // on a low pattern buffer. READ-ONLY on the source. 0 (or no argument) reads and checks the scanout geometry;
    // 1 the POSITIVE CONTROL: kext bars -> pre-flight copy between two scratch VRAM buffers -> SDMA0 QUEUE0 copy into a
    // 256x64 scanout rectangle at (64,96) -> every pixel read back through BAR0; 2 restores that rectangle; 3 READ ONLY
    // content read; 4 READ ONLY thumbnail; 5 (0.0.345, an earlier analysis) the UNARMED DETILE PROOF — three scratch VRAM
    // buffers, never the scanout: the CPU lays down a known ADDR3_64KB_2D image, SDMA detiles it with the new 14-dword
    // COPY_TILED_SUB_WINDOW packet, every pixel is verified, the hardware tiles it back for the cross-check, and a
    // sub-window at a non-zero tiled origin checks the fields a round trip cannot.
    else if (what && !strcmp(what, "scanout")) in = 60;
    // 61 — 0.0.276, `gfxcensus [1|2]` . READ-ONLY census of Apple's GFX frames and the IBs they name, logged
    // at the GFX-ring writeTail, plus the observe hook on AMD 2D contexts (blitCopy/blitFill). 1 arms, 2 disarms, none reads.
    else if (what && !strcmp(what, "gfxcensus")) in = 61;
    // 62 — 0.0.278, `gfxneuter [1|2]` . While armed, every VMID-2 INDIRECT_BUFFER packet of a new frame on
    // Apple's GFX ring is rewritten IN THE RING into a same-length NOP before the doorbell: the frame's wrapper and its own
    // RELEASE_MEM stamp run, the client's stream is DROPPED. 1 arms (refused unless the render drain holds the ring), 2 disarms.
    else if (what && !strcmp(what, "gfxneuter")) in = 62;
    // 63 — 0.0.279, `finishread` . READ-ONLY counters of the 2D context's user-client methods, per process:
    // 0x100 set_surface, 0x101 finish (CoreDisplay's MPHWSync), 0x102 blit.
    else if (what && !strcmp(what, "finishread")) in = 63;
    // 64 — 0.0.282, `gfxcapture [1|2]` . READ-ONLY capture of every GFX submission at the source hook (the
    // gfxneuter hook must be installed) into the kext's binary capture ring: IBs, programs, descriptor tables, pointed-to memory.
    // 1 arms, 2 disarms, none reads. Stream it with `navi48test capstream <file> [interval_ms] [max_s] [stopfile]`.
    else if (what && !strcmp(what, "gfxcapture")) in = 64;
    // 65 — 0.0.283, `gfxprobe [1|2]` . The copy-back probe: for each SecurityAgent submission the capture reads,
    // fill the last draw's VRAM colour target with magenta (IB still neutered). Needs gfxcapture 1. 1 arms, 2 disarms, none reads.
    else if (what && !strcmp(what, "gfxprobe")) in = 65;
    // 66-69 — 0.0.286 (notes sections 491-492, the display brief). `pipeguard [1]` the display-pipe SAFETY CORE (1 arms
    // per-instance refusals on every adopted AMD display pipe and its display object, plus the GFX-ring WRITE_DATA guard; none
    // reads, including the pipe's readiness fields); `agdchold [ms]` arms a ONE-SHOT delay inside our own AGDC reply
// handler on selector 0x921, WindowServer callers only, to open a dtrace attach window the gather does not
// otherwise leave; `agdc [1]` the AppleGraphicsDeviceControl nub (1 publishes, refused
    // without boot-arg navi48-agdc=1 and an armed pipeguard); `fbbench [rows]` in-kernel write throughput into RDNA4FB's
    // scanout per cache mode, writing back the bytes it read; `fbwc [1]` write-combining for RDNA4FB's VRAM mappings.
    else if (what && !strcmp(what, "pipeguard")) in = 66;
    else if (what && !strcmp(what, "agdc")) in = 67;
    else if (what && !strcmp(what, "agdchold")) in = 79;
    else if (what && !strcmp(what, "cqprobe")) in = 80;
    // 81 - 0.0.333 . `ucprobe [0|1|2]`: LOG ONLY. Hooks the accelerator's newUserClient (slot 239)
    // and, for an IOAccelDisplayPipeUserClient2 only, that instance's externalMethod (slot 266), so every external
    // method reaching the display-pipe user client is recorded with its class resolved IN-KERNEL, its selector, the
    // five transactionEnd gate bytes, the ring indices and pipe+0x2a4 before and after. 1 install+log, 2 stop, 0 read.
    else if (what && !strcmp(what, "ucprobe")) in = 81;
    else if (what && !strcmp(what, "fbbench")) in = 68;
    else if (what && !strcmp(what, "fbwc")) in = 69;
    // 82 - 0.0.417 (notes/design/SDMA-DCC-NOPTE.md, D1; section 957). `sdmadcc [0|1|2]`: the SDMA0_DCC_CNTL
    // no-PTE read-decompression / write-compression switch. 0 READ SDMA0_DCC_CNTL (GC[0]+0x34) and
    // SDMA1_DCC_CNTL (GC[0]+0x634) raw and decoded per set; 1 CAPTURE SDMA0's value on first use, write
    // captured & ~0x00015554 (only the eight *_COMP_EN_n bits), read back and report; 2 RESTORE the captured
    // value. Any other argument, and 2 before a capture, is refused. SDMA1 is never written; no other register
    // is written by this verb. The kext log lines are `sdmadcc:`.
    else if (what && !strcmp(what, "sdmadcc")) in = 82;
    // 70-71 - 0.0.288 (an internal review note).
    // `pipeshim [0|1|2|3|4|5]`: the vendor-slot shim on the SAME per-instance vtable copy pipeguard installed. 0 refuses every
    // transaction as before; 1 participates (validate accepts, submit returns kIOReturnNotReady - the only value that
    // reaches perform while skipping the AMD event machine's fence check - and perform censuses each plane and presents
    // nothing); 2 additionally copies plane 0 into RDNA4FB's scanout on SDMA0 QUEUE0. The PM4 flip stays refused in every
    // mode. `pipemode [0|1]`: route B readiness. 0 reads back every readiness field; 1 writes the fields
    // init_framebuffer_resource writes and sets pipe+0x298, running none of AMD's binding code. Needs pipeguard armed.
    else if (what && !strcmp(what, "pipeshim")) in = 70;
    else if (what && !strcmp(what, "pipemode")) in = 71;
    // 72 - 0.0.291 . `emcensus [0|1|2]`: the event-machine census policy toggle, project convention
    // (1 arm, 2 disarm, 0/none read). Disarming drops ONLY the accel+0x380 pass-through counters; every safety-core
    // refusal and the WRITE_DATA->DCN guard stay armed. Call it BEFORE `pipeguard 1`. It isolates cause 2 of the
    // T-compose failure (section 498): whether the census interfered with Metal's present-fence completion.
    else if (what && !strcmp(what, "emcensus")) in = 72;
    // 73 - 0.0.295 (an internal review note). `routea [0|1|2]`: corrected route A, DEFAULT OFF. 1 arms
    // on the safety core's guarded pipes - slot 267 returns a kext-owned VidMemory-shaped object and sets pipe+0x298,
    // and slot 46 (AMDAccelResource::prepare) is neutralised on the framebuffer resource only (per-instance vptr swap of
    // pipe+0xe0). 0 sets the runtime mode off (inert) and reads. Never calls reserveFrameBuffer; needs pipeguard armed.
    else if (what && !strcmp(what, "routea")) in = 73;
    // 74 - 0.0.300 (Track D stage 1, an earlier analysis). `dcnstate [1]`: READ-ONLY DCN 4.1 display state -
    //   which OTG is lit, its frame count, position and OTG_GLOBAL_SYNC_STATUS, the classified DCN interrupt
    //   counters, and the write allowlist's counters and samples. 1 also runs the allowlist self-test in the kernel.
    // 75 - 0.0.300 (T2-VBL). `dcnvbl [0|1|2]`: 0 disable, 1 enable VUPDATE_NO_LOCK on the lit OTG, 2 enable
    //   VSTARTUP. One source at a time; 0 is the escape hatch. Writes only that source's enable/ack bits.
    else if (what && !strcmp(what, "dcnstate")) in = 74;
    else if (what && !strcmp(what, "dcnvbl")) in = 75;
    // 76 - 0.0.301 (T3-FLIP). `dcnflip [0|1|2..30]`: 0 restore the console plane unconditionally (emergency,
    //   idempotent), 1 the machine-verified single-frame flip with an OTG CRC witness, 2..30 hold the test
    //   pattern that many seconds. The hold runs inside the kernel call and a kext watchdog restores on overrun.
    //   0.0.518: `dcnflip 1002..1240` = flip mode's UNARMED A/B test of N = arg - 1000 VUPDATE flips (an earlier analysis,
    //   T-F1); dcnflip 1..30 is refused while flip mode (gfxneuter 74) is ON.
    else if (what && !strcmp(what, "dcnflip")) in = 76;
    // 77 - 0.0.302: `dcnmode [0|1|2..30]` the same-mode re-timing. 0 read-only + emergency restore of the
    //   boot-time capture, 1 write the identical timing set back and verify, 2..30 add a dwell.
    else if (what && !strcmp(what, "dcnmode")) in = 77;
    // 78 - 0.0.308: `fbname [0|1]` the RUNTIME class rename. 1 arms, 0 restores and verifies.
    //   DEFAULT OFF; a wrong offset is a refusal, never a write. Not to be run without an adversarial review.
    else if (what && !strcmp(what, "fbname")) in = 78;   // 0 read-only, 1..30 same-mode, 101..130 REAL v_total change
    // 83..87 - 0.0.613 (#11 11h.2, the display pipe). They exist ONLY when the PC booted with boot-arg navi48-metal-disp=1 (else the kext answers 0xe00002c2: the action bound is 82). All are served by
    // the legacy accel user client, so `pipearm 0` and `fbname 0` work while WindowServer holds the exclusive native N48N connection.
    //   pipeadopt          turn the resource facts on, ask the accelerator for its probe, verify a Navi48DisplayPipe of ours on RDNA4FB, publish the capabilities property
    //   pipearm [0|1]      1 = open the type-4 gate (accel+0xccf) once a verified pipe exists; 0 = close it, ALWAYS allowed (the recovery path)
    //   pipestat [0|1|2|3|4] the counters (page 4 (0.0.618): the vblank timestamps; page 0: hooks and copies, page 1: refusal reasons, page 2: more reasons incl. 0.0.616 not-prepared, and the copy time avg/max, page 3 (0.0.617): scan-owned skips and the submit interval)
    //   pipestamps         READ-ONLY dump of the event machine's stamp words (SUSPECTED layout)
    //   pipeshortcut [0|1] the slot-62 'already prepared' shortcut switch (default 1)
    else if (what && !strcmp(what, "pipeadopt")) in = 83;
    else if (what && !strcmp(what, "pipearm")) in = 84;
    else if (what && !strcmp(what, "pipestat")) in = 85;
    else if (what && !strcmp(what, "pipestamps")) in = 86;
    else if (what && !strcmp(what, "pipeshortcut")) in = 87;
    // 88 - 0.0.614 (#11 11h.3): `pipeagdc [0|1]` - the NATIVE AGDC service (an AppleGraphicsDeviceControl object of ours; IOPresentment's "Unable to get AGDC information" goes away). Same gating as 83..87:
    // boot-arg navi48-metal-disp=1 only. 1 builds and starts it (refused while a row-120 hold is up, or when any check fails); 0 only reads the state.
    else if (what && !strcmp(what, "pipeagdc")) in = 88;
    // 89 - 0.0.618 (V2): `pipevbl [0|1]` - the vblank-timestamp switch. 1 (the default with boot-arg navi48-metal-disp=1) makes the perform hook write the transaction's +0x178 (next vblank, mach_absolute_time units)
    // and +0x188 (that + one refresh period), the words CoreDisplay's SetVBLInfo is fed from; 0 = 0.0.617 behaviour; no argument = read the state. `pipestat 4` has the counts and the last values.
    else if (what && !strcmp(what, "pipevbl")) in = 89;
    // 90 - 0.0.619 (R1): `pipereload [0|1]` - the OPERATOR RESTART WINDOW. With no argument (or 0) it opens a one-shot window of 15 s (kernel uptime clock): inside it the next uid-88 WindowServer client
    // close while armed does NOT auto-disarm (K1) and the restart guard does not count slot-267 calls (K3). It closes when the new WindowServer's first slot 267 has been seen with the pipe still armed,
    // or at 15 s. HUNG (K2) still disarms and is never overridden. 1 only reads the window. OPERATOR SEQUENCE for a deliberate WindowServer restart on an armed pipe:
    //     navi48test accel pipereload        (prints the window: OPEN, 15 s)
    //     killall -9 WindowServer            (within 15 s; no `pipearm 1` dance and no 120 s spacing needed)
    //     navi48test accel pipereload 1      (optional: the window should read closed, "closed by slot 267" 1)
    else if (what && !strcmp(what, "pipereload")) in = 90;
    // 51 — 0.0.239, `shadercache [1|2|3]` (milestone 3 step 2, an earlier analysis).
    // Arms the hash-keyed substitution of gfx1201 code for Apple's GFX10 shaders at
    // the residency copy: kernsub's seam, with kernsub's one hard-coded kernel at one
    // hard-coded offset generalised to a keyed lookup in an embedded cache blob, with
    // a full byte compare before anything is written. NOT kernsub's placement: this one
    // retains no Apple pointer and so can only act while a copy runs — arm it BEFORE the
    // client pages its shaders (right after copyarm), not after. Arming late substitutes
    // nothing and says so. 1 arms, 2 disarms, no argument reads the counters. Boot-arg
    // navi48-shader-cache=1 arms it at start(), which is the production form.
    else if (what && !strcmp(what, "shadercache")) in = 51;
    // 52 — 0.0.242, `vmroots [addr]` (milestone 3 step 4, an earlier analysis). READ-ONLY:
    // it writes nothing into Apple's page tables or anywhere else and reserves no VRAM.
    // Prints every live GCVM_CONTEXTn page-table register; every CONTEXTn base write
    // Apple has queued in its own SDMA rings this boot (no takeover needed — the writes
    // are in ring memory whether or not an engine ran them), folded into the DISTINCT
    // root tables Apple has named; a full 512-entry dump of each of those root pages with
    // every valid entry decoded and the 256 MiB VA range it covers; and the reserved-tail
    // map with arithmetic that adds up. With an address it also dumps that root page.
    else if (what && !strcmp(what, "vmroots")) in = 52;
    // 53 — 0.0.244, `ringmap [0|1]` (milestone 3 step 4, an earlier analysis increments
    // (i) and (ii)). Reserve the VRAM tail region r80 measured free, build the
    // kext-owned page-directory block and its 168 64 KiB leaf entries over the GE ring
    // region, and verify the mapping with OUR OWN walker. IT DOES NOT WRITE INTO APPLE'S
    // PAGE TABLE: the only thing it writes is our own block, in VRAM outside
    // vram_alloc_hi and below Apple's arena, and Apple's root page is read-only here.
    // The single root PDE write that would make the mapping live is increment (iii),
    // held behind adversarial review because Apple recycles a dead client's root page as
    // an ordinary PTE page. No argument reserves and reports only; 1 also builds.
    else if (what && !strcmp(what, "ringmap")) in = 53;
    // 54 — 0.0.247, `vmctx` (milestone 3 step 4, the OBSERVE BOOT of
    // notes/M3-ROOT-WRITE-REVIEW.md section 7.1). READ-ONLY: neither this verb nor the
    // VMM slot-40/41 hooks it reports on write anything — not Apple's page tables, not
    // Apple's objects, not a register, not VRAM. RUN IT WHILE A METAL CLIENT IS ALIVE.
    // It prints, for every AMDHWVMContext Apple created this boot, the root page-table
    // address read out of the context object that owns it (ctx+0x98+0x20) at create,
    // NOW and at release; cross-checks the live value against the root Apple's CONTEXT2
    // register names; reads root[0] and root[511] of each live root (the preconditions
    // of the review's identity guards); and reads GFXHUB engine 17's invalidation range
    // registers. The root at create is EXPECTED to read zero: AMDHWVMContext::init
    // zeroes the page-table control block and the root page is allocated lazily at the
    // first mapVA, so only the on-demand read can be compared with vmroots.
    else if (what && !strcmp(what, "vmctx")) in = 54;
    // 55 — 0.0.250, `rootwrite [0|1]` (milestone 3 step 4 increment (iii)). THE SINGLE
    // 8-BYTE ROOT PDE WRITE: the first write this project has ever made into a live
    // Apple VM page table. It points Apple's VMID-2 root slot 511 (VA 0x23F0000000) at
    // the kext-owned L1 block `ringmap` built, making our GE ring mapping LIVE.
    // NO ARGUMENT (the default) REPORTS ONLY: it evaluates guards G1..G6 against the
    // live state and prints what would happen, writing nothing anywhere. `1` performs
    // the write, and only if all six guards pass. The write is two separate MM-window
    // calls, high dword first, because the helper writes ascending and VALID is bit 0
    // of the low dword. The withdrawal runs on Apple's own teardown path
    // (AMDHWVMContext::pageOffPD, vtable slot 38) at the last instant the page is
    // still ours. RUN IT WHILE A METAL CLIENT IS ALIVE - it needs a live context.
    else if (what && !strcmp(what, "rootwrite")) in = 55;
    // 56 — 0.0.261, `rearmdrain [0|1]`. RE-ARM AFTER THE DRAIN. The trace
    // found that nothing tears VMID 2 down: Apple queues a clearWithDMA of the
    // freshly allocated root block inside the very mapVA our arm writes in, and our
    // own sdmamap drain is what executes it. So the entry is wiped by a packet that
    // was already queued before we armed. 0 REPORTS ONLY and runs instruments C and
    // D (root[511] after the drain, and an UNFILTERED dump of every pending packet
    // touching the root page); 1 also performs the re-arm, through the same single
    // write path and the same six guards. RUN IT WHILE A METAL CLIENT IS ALIVE.
    else if (what && !strcmp(what, "rearmdrain")) in = 56;
    else if (what && strcmp(what, "status") != 0) {
        // REFUSE an unrecognised verb instead of silently running `status`.
        //
        // This bit me on an earlier run: `accel rebaseib` against a stale binary that
        // did not know the verb fell through to in=0, ran status, printed a
        // perfectly healthy-looking "hook installed = yes", and looked for all the
        // world like the driver had refused the rebase. It had simply never been
        // asked. A command that silently does something OTHER than what was typed
        // is worse than one that fails.
        fprintf(stderr, "accel: unknown verb \"%s\"\n", what);
        fprintf(stderr, "  known: status fire memenable synctables enablerings "
                        "startengines ringstate dumpring programqueue ringhooks "
                        "dumpib rebaseib enablequeue kickdoorbell ringrefs queuestate opengate neuterpoll chanstate pokecompletion signalcompletion bindchannel schedstate stampstate signalstamp stampgap runcheckts runadvance xlatregs srbmprobe resume pm4powerup setvspace kiqenable kiqstamp kiqchan gfxmap gfxstate sdmamap sdmastate faultclear vmstate vmib ringib pagecopy flushdrop kernsub vmpage renderxlat eopbridge bootchain shadercache vmroots ringmap vmctx rootwrite rearmdrain pairing drain flushhook scanout gfxcensus gfxneuter finishread gfxcapture gfxprobe pipeguard agdc agdchold cqprobe ucprobe fbbench fbwc pipeshim pipemode emcensus routea dcnstate dcnvbl dcnflip dcnmode fbname sdmadcc pipeadopt pipearm pipestat pipestamps pipeshortcut pipeagdc pipevbl pipereload\n");
        return 2;
    }
    const char *verb = in == 90 ? "pipereload" : in == 89 ? "pipevbl" : in == 88 ? "pipeagdc" : in == 87 ? "pipeshortcut" : in == 86 ? "pipestamps" : in == 85 ? "pipestat" : in == 84 ? "pipearm" : in == 83 ? "pipeadopt" : in == 82 ? "sdmadcc" : in == 81 ? "ucprobe" : in == 80 ? "cqprobe" : in == 79 ? "agdchold" : in == 78 ? "fbname" : in == 77 ? "dcnmode" : in == 76 ? "dcnflip" : in == 75 ? "dcnvbl" : in == 74 ? "dcnstate" : in == 73 ? "routea" : in == 72 ? "emcensus" : in == 71 ? "pipemode" : in == 70 ? "pipeshim" : in == 69 ? "fbwc" : in == 68 ? "fbbench" : in == 67 ? "agdc" : in == 66 ? "pipeguard" : in == 65 ? "gfxprobe" : in == 64 ? "gfxcapture" : in == 63 ? "finishread" : in == 62 ? "gfxneuter" : in == 61 ? "gfxcensus" : in == 60 ? "scanout" : in == 59 ? "flushhook" : in == 58 ? "drain" : in == 57 ? "pairing" : in == 56 ? "rearmdrain" : in == 55 ? "rootwrite" : in == 54 ? "vmctx" : in == 53 ? "ringmap" : in == 52 ? "vmroots" : in == 51 ? "shadercache" : in == 50 ? "bootchain" : in == 49 ? "eopbridge" : in == 48 ? "renderxlat" : in == 47 ? "vmpage" : in == 46 ? "kernsub" : in == 45 ? "flushdrop" : in == 44 ? "pagecopy" : in == 43 ? "ringib" : in == 42 ? "vmib" : in == 41 ? "vmstate" : in == 40 ? "faultclear"
                     : in == 39 ? "sdmastate" : in == 38 ? "sdmamap"
                     : in == 37 ? "gfxstate" : in == 36 ? "gfxmap"
                     : in == 35 ? "kiqchan" : in == 34 ? "kiqstamp" : in == 33 ? "kiqenable" : in == 32 ? "setvspace" : in == 31 ? "pm4powerup" : in == 30 ? "resume" : in == 29 ? "srbmprobe" : in == 28 ? "xlatregs" : in == 27 ? "runadvance" : in == 26 ? "runcheckts" : in == 25 ? "stampgap" : in == 24 ? "signalstamp" : in == 23 ? "stampstate" : in == 22 ? "schedstate" : in == 21 ? "bindchannel" : in == 20 ? "signalcompletion" : in == 19 ? "pokecompletion" : in == 18 ? "chanstate" : in == 17 ? "neuterpoll" : in == 16 ? "opengate" : in == 15 ? "queuestate" : in == 14 ? "ringrefs" : in == 13 ? "kickdoorbell" : in == 12 ? "enablequeue" : in == 11 ? "rebaseib" : in == 10 ? "dumpib" : in == 9 ? "ringhooks" : in == 8 ? "programqueue" : in == 7 ? "dumpring" : in == 6 ? "ringstate" : in == 5 ? "startengines" : in == 4 ? "enablerings" : in == 3 ? "synctables"
                     : in == 2 ? "memenable" : (in ? "fire" : "status");
    // out[0..2] are the long-standing three; out[3..15] are verb-specific extras
    // (rule 14: a result that matters must not depend on the shared log buffer).
    // 16 is the hard ABI limit — io_scalar_inband64_t is uint64_t[16].
    uint64_t out[16] = {0};
    uint32_t outCnt = 16;
    // 0.0.194: scalarInput[1] is a verb argument — `vmib`'s VA. Always sent, so
    // a kext that ignores it is unaffected; 0 means "use the verb's default".
    uint64_t in2[2] = { in, 0 };
    if (arg) in2[1] = strtoull(arg, NULL, 0);
    // 0.0.618 review: `pipevbl` with no argument would send 0 and turn the vblank timestamps OFF; require 0 or 1.
    if (in == 89 && !arg) { fprintf(stderr, "accel pipevbl: give 0 or 1 explicitly (read the state with `accel pipestat 4`)\n"); return 2; }
    // 0.0.416 (notes/design/SDMA-GCR.md, G2): mode 7 carries its source VRAM offset as a SECOND CLI token
    // (`accel scanout 7 <vramOff> [gcr]`) and packs it into the ONE ABI scalar with the mode and the GCR flag
    // (sdma_gcr.h). All other verbs ignore arg2/arg3.
    // build 0.0.542: `accel scanout full [file]` is mode 9 (the optional file is written by scanfull_pull below).
    if (in == 60 && arg && !strcmp(arg, "full")) in2[1] = N48_SF_MODE;
    if (in == 60 && arg && strtoull(arg, NULL, 0) == 7) {
        const uint64_t off = arg2 ? strtoull(arg2, NULL, 0) : 0;
        const int gcr = (arg3 && !strcmp(arg3, "gcr")) ? 1 : 0;
        in2[1] = n48_scanout7_scalar(off, gcr);
    }
    kern_return_t kr = IOConnectCallScalarMethod(conn, kNavi48SelAccelExperiment,
                                                 in2, 2, out, &outCnt);
    if (kr != KERN_SUCCESS) {
        printf("accel %s: FAILED (0x%x)%s\n", verb, kr,
               kr == kIOReturnNotPermitted
                   ? " — not armed, OR (0.0.236, `fire`) the EXPOSURE GATE is closed: LoadAccelerator is"
                     " withheld so no accelerator nub and no Metal device appear. Read Navi48,AccelGate in"
                     " ioreg and the accel-gate lines in `log`. Boot the live variant stage17-accel-pdb0-qn"
                     " , never plain stage17-accel"
                   : (kr == kIOReturnNotReady ? " — the TTL hook did not install; see `log`" : ""));
        return 1;
    }
    printf("accel %s: hook installed = %s\n", verb, out[0] ? "yes" : "no");
    printf("  TTL calls so far        : %llu\n", (unsigned long long)out[1]);
    if ((int64_t)out[2] >= 0)
        printf("  first unsupported slot  : %lld\n", (long long)(int64_t)out[2]);
    else
        printf("  first unsupported slot  : none yet\n");
    if (in == 57) {
        // 0.0.267 extras : out[3 + i] = v[i], v as navi48_pairing_control fills it.
        static const char *vn[] = { "read", "ENABLED for this boot", "DISABLED for this boot",
                                    "WITHDRAW", "REFUSED: too late (decided at accelerator start)",
                                    "REFUSED: bad argument", "already withdrawn" };
        static const char *sn[] = { "OFF by default", "ON by boot-arg", "OFF by boot-arg",
                                    "ON by verb", "OFF by verb" };
        static const char *wr[] = { "withdrawn, read back absent", "nothing of ours stamped",
                                    "REFUSED: IOAccelTypes is not the path we stamped",
                                    "a key is STILL PRESENT after removal" };
        const unsigned long long vd = out[3], src = out[8], wrs = out[11];
        printf("  pairing verdict         : %llu (%s)\n", vd, vd <= 6 ? vn[vd] : "?");
        printf("  verb request            : %llu (0 none, 1 enable, 2 disable)\n", (unsigned long long)out[4]);
        printf("  boot-arg                : %s%llu\n", out[5] ? "navi48-display-pairing=" : "absent ",
               (unsigned long long)out[6]);
        printf("  decided at accel start  : %llu%s%s\n", (unsigned long long)out[7],
               out[7] ? " - " : "", out[7] ? (src <= 4 ? sn[src] : "?") : "");
        printf("  keys stamped / withdrawn: %llu / %llu\n", (unsigned long long)out[9],
               (unsigned long long)out[10]);
        if (vd == 3) printf("  withdraw result         : %llu (%s)\n", wrs, wrs <= 3 ? wr[wrs] : "?");
        printf("  peer present            : %llu\n", (unsigned long long)out[12]);
        if (vd == 4 || (vd == 3 && wrs != 0)) return 3;
    }
    if (in == 62) {
        // 0.0.279 packing : out[3] armed | source install state << 8, out[6] ring mismatches | not-IB << 20 |
        // over cap << 40, out[9] source refusals | walk-stopped << 32, out[12] drain state | published-before-hook << 32
        printf("  neuter armed / frames   : %llu / %llu (IB packets NOPed %llu, read-back mismatches %llu)\n", (unsigned long long)(out[3] & 0xff),
               (unsigned long long)out[4], (unsigned long long)out[5], (unsigned long long)(out[6] & 0xfffff));
        printf("  source neuter           : install %llu (1 ok 2 refused); submissions %llu, IBs %llu, refused %llu\n",
               (unsigned long long)(out[3] >> 8), (unsigned long long)out[7], (unsigned long long)out[8], (unsigned long long)(out[9] & 0xffffffffull));
        printf("  not-an-IB / over cap    : %llu / %llu; walk-stopped frames %llu; raced frames %llu\n", (unsigned long long)((out[6] >> 20) & 0xfffff),
               (unsigned long long)(out[6] >> 40), (unsigned long long)(out[9] >> 32), (unsigned long long)(out[15] >> 32));
        printf("  published before hook   : %llu\n", (unsigned long long)(out[12] >> 32));
        /* 0.0.359 : out[10] = CP_RB0_RPTR | the kext's trusted CP ring size in dwords << 32 (0 = unknown,
           and always 0 from a kext before 0.0.359). RPTR is an offset INTO the ring and WPTR is Apple's unwrapped count, so
           "caught up" is equality MODULO THE RING (hp1 printed CP BEHIND for 0xe380 / 0x8e380, a caught-up CP on a wrapped
           ring). Without a ring size an unequal pair is reported as undecidable, never as BEHIND. */
        {
            const unsigned rp = (unsigned)(out[10] & 0xffffffffull), ring = (unsigned)(out[10] >> 32);
            const unsigned wp = (unsigned)(out[11] & 0xffffffffull);
            const int pow2 = ring != 0 && (ring & (ring - 1u)) == 0;
            if (pow2)
                printf("  CP_RB0_RPTR / WPTR      : %#x / %#x (ring %#x dw, WPTR mod ring %#x: %s)\n", rp, wp, ring,
                       wp & (ring - 1u), n48_ring_caught_up(rp, wp, ring) ? "CP caught up" : "CP BEHIND");
            else
                printf("  CP_RB0_RPTR / WPTR      : %#x / %#x (%s)\n", rp, wp, rp == wp ? "CP caught up"
                       : "ring size not reported - RPTR != WPTR can be a WRAP, not a lag; undecidable");
        }
        printf("  render drain / walked   : state %llu / %llu GFX frame(s); last IB VA %#llx len %llu\n", (unsigned long long)(out[12] & 0xffffffffull),
               (unsigned long long)out[13], (unsigned long long)out[14], (unsigned long long)(out[15] & 0xffffffffull));
    }
    if (in == 66) {
        // 0.0.286 : out[3 + i] = v[i] of navi48_pipeguard_control.
        static const char *ps[] = { "ARMED", "no accelerator", "slide", "display machine identity", "framebuffer count",
                                    "pipe identity", "pipe slot / partial", "display identity", "display slot", "allocation",
                                    "WRITE_DATA guard refused (no GFX writeTail hook)", "no adopted framebuffer yet",
                                    "foreign vtable", "bad argument", "event machine not AMDAccelEventMachine",
                                    "event machine slot identity", "event machine alloc", "event machine vptr swap" };
        const unsigned long long st = out[3], ps0 = out[13];
        printf("  pipeguard status        : %llu (%s)\n", st, st <= 17 ? ps[st] : "?");
        printf("  armed / verified now    : %llu / %llu\n", out[4] & 1, (out[4] >> 1) & 1);
        printf("  event-machine census    : armed %llu / verified %llu / total slot hits %llu (accel+0x380; 0 in T-ready is expected)\n",
               (out[4] >> 2) & 1, (out[4] >> 3) & 1, (out[5] >> 48) & 0xffff);
        printf("  pipes / displays / fbs  : %llu / %llu / %llu\n", out[5] & 0xffff, (out[5] >> 16) & 0xffff, (out[5] >> 32) & 0xffff);
        printf("  REFUSED initFbResource  : %llu (destroy passed %llu)\n", out[6] & 0xffffffffull, out[6] >> 32);
        printf("  txn irq enable / disable: %llu / %llu\n", out[7] & 0xffffffffull, out[7] >> 32);
        /* 0.0.337 : this line used to read "REFUSED validate/perform" and printed gPg.validate /
           gPg.perform, which increment at the top of the hook BEFORE the refusal test - they are ENTRY counts, and
           with the shim armed every one of them is an ACCEPTANCE. The kext now sends the accepted counts in the top
           half of each word, so the refusals are DERIVED here rather than asserted. */
        {
            const unsigned long long vc = out[8] & 0xffffffffull, va = out[8] >> 32;
            const unsigned long long pc = out[9] & 0xffffffffull, pa = out[9] >> 32;
            printf("  validate calls/acc/REF  : %llu / %llu / %llu\n", vc, va, vc >= va ? vc - va : 0);
            printf("  perform  calls/acc/REF  : %llu / %llu / %llu\n", pc, pa, pc >= pa ? pc - pa : 0);
        }
        printf("  isComplete / submit     : %llu / %llu\n", out[10] & 0xffffffffull, out[10] >> 32);
        printf("  begin / signal          : %llu / %llu\n", out[11] & 0xffffffffull, out[11] >> 32);
        printf("  REFUSED FLIPS           : %llu\n", out[12]);
        printf("  pipe[0] readiness       : active(+0x298) %llu defer(+0x282) %llu fbIndex %llu accelEnabled %llu userClient %llu fbIsRDNA4FB %llu\n",
               ps0 & 0xff, (ps0 >> 8) & 0xff, (ps0 >> 16) & 0xffff, (ps0 >> 32) & 1, (ps0 >> 33) & 1, (ps0 >> 34) & 1);
        printf("  WRITE_DATA walked / DCN : %llu / %llu\n", out[14] & 0xffffffffull, out[14] >> 32);
        printf("  DCN NOPed / mismatches  : %llu / %llu\n", out[15] & 0xffffffffull, out[15] >> 32);
        /* 0.0.335 : the VBL half of pipe[0] state, which 0.0.334 packed into ps0 but never
           printed - so the only place it could be read was the driver log, which a hang can truncate. */
        printf("  pipe[0] VBL             : +0xd8 cookie non-NULL %llu (registerForInterruptType('vbl ') %s)"
               " +0x2a0 arm %llu +0x281 txnOverVbl %llu\n",
               (ps0 >> 35) & 1, ((ps0 >> 35) & 1) ? "SUCCEEDED" : "NEVER SUCCEEDED", (ps0 >> 36) & 0xff, (ps0 >> 44) & 1);
        printf("  pipe[0] TransactIR      : +0x2a4 %llu%s (notify list head %llu, NOT EMPTY %llu; Pending/Live set %llu)\n",
               (ps0 >> 45) & 0xffff, ((ps0 >> 45) & 0xffff) ? "  *** NON-ZERO ***" : "",
               (ps0 >> 61) & 1, (ps0 >> 62) & 1, (ps0 >> 63) & 1);
    }
    if (in == 81) {
        /* 0.0.335 : ucprobe's out-scalars were never printed - section 684's counts came
           from the driver log alone. Print them, so a truncated log cannot lose the answer. */
        const unsigned long long c6 = out[9], c10 = out[13], c12 = out[15];
        const unsigned long long lastSlot = (c10 >> 36) & 0xf;
        printf("  installed / mode        : %llu / %llu\n", out[4] & 1, out[4] >> 32);
        printf("  clients minted / foreign: %llu / %llu (failed %llu)\n",
               out[6] & 0xffffffffull, out[6] >> 32, out[7] & 0xffffffffull);
        printf("  display-pipe seen/patch : %llu / %llu (guard refusals %llu)\n",
               out[8] & 0xffffffffull, out[8] >> 32, c6 & 0xffff);
        printf("  externalMethod calls    : %llu; selector bitmask %#llx\n", out[10], out[12]);
        printf("  sel 4 transaction_begin : %llu     sel 8 transaction_end: %llu\n",
               out[11] & 0xffffffffull, out[11] >> 32);
        printf("  sel 3 request_notify    : %llu call(s) = slot 0 (ARMING, uc+0xf8) %llu + slot 1 (NEVER ARMS, uc+0x140) %llu\n",
               (c6 >> 16) & 0xffff, (c6 >> 32) & 0xffff, (c6 >> 48) & 0xffff);
        printf("  sel 3 with cookie live  : %llu of those arrived with pipe+0xd8 ALREADY non-NULL\n", (c10 >> 56) & 0xff);
        printf("  last sel 3 slot / kind  : %s / record+0x40 = %llu\n",
               lastSlot == 0 ? "(none seen)" : lastSlot == 1 ? "0 (ARMING)" : lastSlot == 2 ? "1 (NEVER ARMS)" : "out of range",
               (c10 >> 40) & 0xff);
        printf("  last sel 3 cookie b/a   : %llu / %llu     pipe+0x2a0 arm before/after: %llu / %llu\n",
               (c10 >> 32) & 1, (c10 >> 33) & 1, (c10 >> 34) & 1, (c10 >> 35) & 1);
        printf("  max pipe+0x2a0 seen     : %llu (1 = THE VBL ARM HAPPENED); cookie ever non-NULL %llu\n",
               (c10 >> 48) & 0xff, c12 >> 32);
        printf("  calls with +0x2a4 after : %llu; last sel %llu kr %#llx +0x2a4 %llu\n",
               c10 & 0xffffffffull, out[14] & 0xffffffffull, out[14] >> 32, c12 & 0xffffffffull);
    }
    if (in == 67) {
        static const char *as[] = { "PUBLISHED", "boot-arg navi48-agdc=1 absent", "safety core not armed on every pipe",
                                    "AGDC class not loaded", "slide", "class size", "code bytes", "vtable", "framebuffer / PCI",
                                    "no AppleGPUWrangler", "allocation", "start failed", "already published", "bad argument" };
        const unsigned long long st = out[3];
        printf("  agdc status             : %llu (%s)\n", st, st <= 13 ? as[st] : "?");
        printf("  published / start ok    : %llu / %llu\n", out[4] & 1, (out[4] >> 1) & 1);
        printf("  registry ID             : %#llx\n", out[5]);
        printf("  vendor calls            : %llu (vendor info %llu, GPU capability %llu, other %llu, bad length %llu)\n", out[6],
               out[7] & 0xffffffffull, out[7] >> 32, out[8] & 0xffffffffull, out[8] >> 32);
        for (unsigned i = 9; i < 16; i++)
            if (out[i]) printf("  command %#llx x%llu\n", out[i] >> 32, out[i] & 0xffffffffull);
    }
    if (in == 68) {
        static const char *mn[] = { "default (0x000)", "inhibit (0x100)", "write-thru (0x200)", "copyback (0x300)", "write-combine (0x400)" };
        printf("  fbbench status          : %llu (0 ok, 1 no RDNA4FB, 2 geometry, 3 alloc)\n", out[3]);
        printf("  bytes per copy          : %llu\n", out[4]);
        printf("  RAM -> RAM              : %llu KiB/s\n", out[5]);
        for (unsigned m = 0; m < 5; m++)
            printf("  %-24s: WRITE %llu KiB/s (IOMemoryDescriptor kernel mapping)\n", mn[m], out[6 + m]);
        printf("  snapshot READ           : %llu KiB/s\n", out[11]);
        printf("  ml_io_map (uncached)    : WRITE %llu KiB/s\n", out[14]);
        printf("  ml_io_map_wcomb (WC)    : WRITE %llu KiB/s\n", out[15]);
    }
    if (in == 69) {
        printf("  fbwc status             : %llu (0 ok, 2 kernel slide, 3 descriptor vtable, 4 no RDNA4FB, 5 geometry, 6 slot 310, 7 alloc, 8 swap)\n", out[3]);
        printf("  armed / boot-arg / notif: %llu / %llu / %llu\n", out[4] & 1, (out[4] >> 1) & 1, (out[4] >> 2) & 1);
        printf("  getVRAMRange / wrapped  : %llu / %llu (refused %llu)\n", out[5], out[6], out[7]);
        printf("  doMap / WC forced / kept: %llu / %llu / %llu\n", out[8], out[9], out[10]);
    }
    if (in == 70) {
        // 0.0.288: out[3 + i] = v[i] of navi48_pipeshim_control.
        printf("  pipeshim status         : %llu (0 ok, 1 safety core not armed, 2 ARG out of range, 3 mode 2/4/5 refused:\n"
               "                            scanout positive control not passed.  ARG 3 = READ ONLY, 0.0.338 -- a bare\n"
               "                            `accel pipeshim` sends scalar 0, which TURNS THE SHIM OFF. ARG 4 = forced linear,\n"
               "                            ARG 5 = tiled copy + GCR_REQ, both 0.0.416/0.0.412)\n", out[3]);
        printf("  mode / pipeguard armed  : %llu (0 refuse, 1 census, 2 present) / %llu; GCR_REQ %s; tiled copy %u dwords\n",
               out[4] & 0xff, (out[4] >> 8) & 1, ((out[4] >> 9) & 1) ? "ON" : "off",
               ((out[4] >> 9) & 1) ? 23u : 18u);
        printf("  validate / perform      : %llu / %llu\n", out[5], out[6]);
        printf("  submit / isComplete     : %llu / %llu\n", out[7] & 0xffffffffULL, out[7] >> 32);
        printf("  census / usable planes  : %llu / %llu\n", out[8] & 0xffffffffULL, out[8] >> 32);
        // 0.0.373: three buckets, and the line says what it is. A `pipeshimread` mid-stream is a
        // SNAPSHOT, not a boot total (arm3 reported 6 against a boot that performed 15), and a readback
        // disagreement is NOT a refusal - the copy was built, submitted and fenced, and the pixels reached the
        // scanout; what disagreed was our own detile-equation check of rows 0 and h-1 afterwards.
        printf("  presents (SNAPSHOT, not a boot total - see the driver log's HEARTBEAT for the boot's last word)\n");
        printf("    ok / readback-differed  : %llu / %llu   (DELIVERED %llu)\n", out[9] & 0xffffffffULL,
               out[15] >> 32, (out[9] & 0xffffffffULL) + (out[15] >> 32));
        printf("    refused (no copy made)  : %llu\n", out[9] >> 32);
        printf("  last copy status / us   : %#llx / %llu\n", out[10] & 0xffffffffULL, out[10] >> 32);
        printf("  last plane / VRAM       : %llux%llu at %#llx +%#llx\n", out[11] >> 32, out[11] & 0xffffffffULL, out[12], out[13]);
        printf("  ring submit / retire    : %llu / %llu (depth %lld of 4; wait_for_queue_slot blocks at 4)\n",
               out[14] & 0xffffffffULL, out[14] >> 32, (long long)(out[14] & 0xffffffffULL) - (long long)(out[14] >> 32));
        printf("  live txn / 0x2a4 / ready: %llu / %llu / %llu\n", out[15] & 1, (out[15] >> 1) & 1, (out[15] >> 2) & 1);
        // 0.0.331: the five transactionEnd (selector 8) gates in one line. The RAW bytes are in the
        // driver log ("pipeshim: TXEND GATES"); these are the booleans. The ABI is full at 16 scalars, so they
        // ride in the spare bits of the same word rather than in new slots.
        printf("  TXEND gates 5/5         : accel+0xc78&2 %llu | pipe+0x280 %llu, +0x282 %llu, +0x298 %llu, +0x299 %llu"
               "  (any one 0 -> kIOReturnNotReady 0xe00002d8; see `log` for the raw bytes)\n",
               (out[15] >> 3) & 1, (out[15] >> 4) & 1, (out[15] >> 5) & 1, (out[15] >> 2) & 1, (out[15] >> 6) & 1);
    }
    if (in == 71) {
        // 0.0.288: out[3 + i] = v[i] of navi48_pipemode_control.
        printf("  pipemode status         : %llu (0 done, 1 accelerator/slide, 2 display machine, 3 no pipe, 4 pipe identity,\n"
               "                            5 pipe+0xe0, 6 not RDNA4FB, 7 getCurrentDisplayMode, 8 getPixelInformation,\n"
               "                            9 not 32 bpp, 10 mode/Console disagree, 11 argument, 12 safety core not armed)\n", out[3]);
        printf("  framebuffer mode / depth: %lld / %lld\n", (long long)(int)(uint32_t)out[4], (long long)(int)(uint32_t)(out[4] >> 32));
        printf("  mode geometry           : %llux%llu, %llu bpp, %llu bits per component, rowBytes %llu\n",
               out[5] & 0xffffULL, (out[5] >> 16) & 0xffffULL, (out[5] >> 32) & 0xffULL, (out[5] >> 40) & 0xffULL, out[13]);
        printf("  Console,* geometry      : %llux%llu, rowBytes %llu\n", out[14] & 0xffffULL, (out[14] >> 16) & 0xffffULL, out[14] >> 32);
        printf("  pipe +0x28c fmt / width : %llu / %llu\n", out[6] & 0xffffffffULL, out[6] >> 32);
        printf("  pipe height/bpp8/READY  : %llu / %llu / %llu (defer %llu)\n", out[7] & 0xffffULL, (out[7] >> 16) & 0xffffULL,
               (out[7] >> 32) & 0xffULL, (out[7] >> 40) & 0xffULL);
        printf("  resource w/h/bytes-px   : %llu / %llu / %llu\n", out[8] & 0xffffULL, (out[8] >> 16) & 0xffffULL, (out[8] >> 32) & 0xffffULL);
        printf("  resource rowBytes/bytes : %llu / %llu\n", out[9], out[10]);
        printf("  resource +0x88 VidMemory: %#llx (left untouched by route B)\n", out[11]);
        printf("  display resource        : %#llx\n", out[12]);
        printf("  pipeguard armed / shim  : %llu / %llu\n", out[15] & 1, (out[15] >> 8) & 0xff);
    }
    if (in == 72) {
        // 0.0.291 : out[3 + i] = v[i] of navi48_emcensus_control.
        printf("  emcensus status         : %llu (0 ok, 1 refused - safety core already armed, 2 arg out of range)\n", out[3]);
        printf("  census policy / armed   : %s / armed %llu verified %llu (safety core armed %llu)\n",
               (out[4] & 1) ? "ARMED" : "DISARMED", (out[4] >> 1) & 1, (out[4] >> 2) & 1, (out[4] >> 3) & 1);
        printf("  total slot hits         : %llu\n", out[5]);
        printf("  initEvent / cleanEvent  : %llu / %llu\n", out[6] & 0xffffffffULL, out[6] >> 32);
        printf("  testEvent / copyEvent   : %llu / %llu\n", out[7] & 0xffffffffULL, out[7] >> 32);
        printf("  mergeEvent / enStamp    : %llu / %llu\n", out[8] & 0xffffffffULL, out[8] >> 32);
    }
    if (in == 73) {
        // 0.0.295 (an internal review note): out[3 + i] = v[i] of navi48_routea_control.
        printf("  routea status           : %llu (0 ok, 1 safety core not armed, 2 accel/slide, 3 display machine, 4 pipe,\n"
               "                            5 resource vtable, 6 not RDNA4FB, 7 geometry, 8 alloc, 9 vptr swap, 10 arg)\n", out[3]);
        printf("  armed / mode / verified : %llu / %llu / %llu (pipeguard armed %llu)\n",
               out[4] & 1, (out[4] >> 1) & 1, (out[4] >> 2) & 1, (out[4] >> 3) & 1);
        printf("  pipes / fmt / bpp       : %llu / %llu / %llu\n", out[5] & 0xffffULL, (out[5] >> 16) & 0xffULL, (out[5] >> 24) & 0xffULL);
        printf("  object frame            : %llu x %llu\n", (out[5] >> 32) & 0xffffULL, (out[5] >> 48) & 0xffffULL);
        printf("  scanout phys / obj len  : %#llx / %#llx\n", out[6], out[7]);
        printf("  slot267 initFb / ready  : %llu / %llu\n", out[8] & 0xffffffffULL, out[8] >> 32);
        printf("  slot46 guard / passthru : %llu / %llu\n", out[9] & 0xffffffffULL, out[9] >> 32);
        printf("  obj release / physSeg   : %llu / %llu\n", out[10] & 0xffffffffULL, out[10] >> 32);
        printf("  shim bypasses / idMiss  : %llu / %llu\n", out[11] & 0xffffffffULL, out[11] >> 32);
        printf("  object vt / res copy    : %#llx / %#llx\n", out[12], out[13]);
        printf("  real prepare (passthru) : %#llx\n", out[14]);
    }
    if ((in >= 83 && in <= 87) || in == 89 || in == 90) {
        // 0.0.613: out[3 + i] = v[i] of n48disp_verb; v[0] is the status (n48disp::Status), the rest is verb specific.
        static const char *sn[] = { "OK", "bad argument", "display is OFF (boot-arg navi48-metal-disp is not 1)", "no Navi48Accelerator under our nub (publish the nub first)",
                                    "the accelerator object failed its positive controls", "the resource fact bits are not on", "the display machine already holds pipes that are not a verified Navi48DisplayPipe",
                                    "no pipe exists", "the pipe is not a Navi48DisplayPipe of ours", "the pipe does not belong to this accelerator / display machine / RDNA4FB", "requestProbe did not succeed",
                                    "already adopted (verified again)", "the write did not read back", "accel+0x378 is not a Navi48DisplayMachine", "the capabilities property could not be set",
                                    "another display verb is running", "accel+0x380 is not a Navi48EventMachine",
                                    "the family stored a NULL pipe (its init failed)" };
        const unsigned long long st = out[3];
        printf("  status                  : %llu (%s)\n", st, st <= 17 ? sn[st] : "?");
        if (in == 83 && st == 17) printf("  ADVICE                  : NULL pipe stored by the family; REBOOT before any WindowServer restart (any later display-machine loop dereferences it)\n");
        const unsigned long long fl = out[4];
        if (in == 83) {
            printf("  flags                   : %#llx (1 display ON, 2 adopted, 4 armed, 8 capabilities, 16 shortcut ON, 32 BAR0 kernel mapping write-combined)\n", (unsigned long long)out[12]);
            printf("  BAR0 kernel mapping     : %s\n", (out[12] & 32) ? "WRITE-COMBINED (perform's CPU copy can run at frame rate)" : "UNCACHED (perform would copy at ~45 MiB/s: do NOT run anything that reaches performTransaction)");
            printf("  factory mask now        : %#llx (needs 0xd = RESOURCE|SYSMEMORY|VIDMEMORY)\n", fl);
            printf("  adopted pipe            : %#llx  (accelerator %#llx)\n", (unsigned long long)out[5], (unsigned long long)out[11]);
            printf("  aux traces: pipes ours / all, display-machine starts / PCI substituted: %llu / %llu, %llu / %llu (last walk provider %#llx)\n", (unsigned long long)out[6], (unsigned long long)out[10],
                   (unsigned long long)out[7], (unsigned long long)out[8], (unsigned long long)out[9]);
        } else if (in == 84) {
            printf("  flags                   : %#llx (1 display ON, 2 adopted, 4 armed, 8 capabilities, 16 shortcut ON, 32 BAR0 kernel mapping write-combined)\n", fl);
            printf("  BAR0 kernel mapping     : %s\n", (fl & 32) ? "WRITE-COMBINED" : "UNCACHED (an armed run that reaches perform would copy at ~45 MiB/s)");
            printf("  accel+0xccf now         : %llu (255 = not readable)\n", (unsigned long long)out[5]);
            printf("  arms / disarms          : %llu / %llu; factory mask %#llx; adopted pipe %#llx\n", (unsigned long long)out[6], (unsigned long long)out[7], (unsigned long long)out[8], (unsigned long long)out[9]);
        } else if (in == 85) {
            printf("  flags / factory mask    : %#llx / %#llx\n", fl, (unsigned long long)out[5]);
            if (fl & 64) {   /* 0.0.617 (K4): the pipe was disarmed WITHOUT the operator; bits 8..11 = the cause; only an explicit `pipearm 1` re-arms it */
                static const char *cn[] = { "none", "WindowServer restart loop (slot 267 x3 within 120 s)", "the WindowServer GPU client closed while armed", "the GPU was declared HUNG while armed" };
                const unsigned c = (unsigned)((fl >> 8) & 15u);
                printf("  auto-disarmed           : %s (re-arm needs an explicit `pipearm 1`)\n", c < 4u ? cn[c] : "?");
            }
            printf("  BAR0 kernel mapping     : %s\n", (fl & 32) ? "WRITE-COMBINED" : "UNCACHED (an armed run that reaches perform would copy at ~45 MiB/s)");
            if (!arg || strtoull(arg, NULL, 0) == 0) {
                printf("  hook calls 267/277/278/279: %llu / %llu / %llu / %llu; handled %llu / %llu / %llu / %llu\n", (unsigned long long)out[6], (unsigned long long)out[7], (unsigned long long)out[8], (unsigned long long)out[9],
                       (unsigned long long)(out[10] & 0xffff), (unsigned long long)((out[10] >> 16) & 0xffff), (unsigned long long)((out[10] >> 32) & 0xffff), (unsigned long long)((out[10] >> 48) & 0xffff));
                printf("  submit pass-through / will-perform: %llu / %llu\n", (unsigned long long)out[11], (unsigned long long)out[12]);
                printf("  bytes copied / frames copied / completed-disarmed: %llu / %llu / %llu\n", (unsigned long long)out[13], (unsigned long long)out[14], (unsigned long long)out[15]);
            } else if (strtoull(arg, NULL, 0) == 1) {
                static const char *rn[] = { "no plane 0", "txn not kernel ptr", "plane array bad", "IOSurface/resource bad", "resource fields bad", "source memory bad", "source not admitted", "geometry disagrees", "console unknown", "geometry refused" };
                for (int i = 0; i < 10; i++) printf("  refused: %-24s: %llu\n", rn[i], (unsigned long long)out[6 + i]);
            } else if (strtoull(arg, NULL, 0) == 3) {
                /* 0.0.617 (K6): page 3 - kernel out[3..9] arrive as out[6..12] here */
                printf("  perform skipped, scanout plane owned by a native client: %llu\n", (unsigned long long)out[6]);
                printf("  submit not wired, scanout plane owned                  : %llu\n", (unsigned long long)out[7]);
                printf("  submit interval ns min/avg/max (n intervals)           : %llu / %llu / %llu (%llu)\n", (unsigned long long)out[9], (unsigned long long)out[10], (unsigned long long)out[11], (unsigned long long)out[8]);
                printf("  scanout plane owned by a native client right now       : %s\n", out[12] ? "YES (the v1 copy is off)" : "no");
            } else if (strtoull(arg, NULL, 0) == 4) {
                /* 0.0.618 (V2): page 4 - kernel out[3..12] arrive as out[6..15] here */
                printf("  vblank timestamp switch                                : %s\n", out[6] ? "ON" : "OFF");
                printf("  stamps written (txn+0x178 / +0x188)                    : %llu\n", (unsigned long long)out[7]);
                printf("  skipped: switch off + pipe disarmed                    : %llu\n", (unsigned long long)out[8]);
                printf("  skipped: bad transaction pointer + wrong class         : %llu\n", (unsigned long long)out[9]);
                printf("  skipped: no coherent OTG sample + HUNG                 : %llu\n", (unsigned long long)out[10]);
                printf("  skipped: arithmetic refused + write refused            : %llu\n", (unsigned long long)out[11]);
                printf("  last t_vbl (mach abs) / period ns / to-next-vblank ns / period abs: %llu / %llu / %llu / %llu\n", (unsigned long long)out[12], (unsigned long long)out[13], (unsigned long long)out[14], (unsigned long long)out[15]);
            } else {
                static const char *rn[] = { "stride refused", "dest rows past console", "source rows past source", "a copy was running", "no scratch buffer", "short source read", "console write refused", "NOT PREPARED (cache miss)" };
                for (int i = 0; i < 8 && 6 + i < 14; i++) printf("  refused: %-24s: %llu\n", rn[i], (unsigned long long)out[6 + i]);   /* 0.0.616: reason 19 = out[13]; the copy-time minimum moved to the kext log line */
                printf("  copy time ns avg/max    : %llu / %llu (copied frames only; the minimum is in the driver log's `pipe stat` line)\n", (unsigned long long)out[14], (unsigned long long)out[15]);
            }
        } else if (in == 89) {
            printf("  vblank timestamps       : %s\n", fl ? "ON (perform writes txn+0x178 / +0x188)" : "OFF (0.0.617 behaviour)");
            printf("  written / last period ns: %llu / %llu\n", (unsigned long long)out[5], (unsigned long long)out[6]);
            printf("  last t_vbl / next (abs) : %llu / %llu\n", (unsigned long long)out[7], (unsigned long long)out[8]);
        } else if (in == 90) {
            /* 0.0.619 (R1): out[3 + i] = v[i]: v[1] state, v[2] ms left, v[3] opened, v[4] closes tolerated, v[5] closed by slot 267, v[6] expired, v[7] armed, v[8] HUNG */
            static const char *wn[] = { "closed", "OPEN, waiting for the old WindowServer client's close", "OPEN, close tolerated, waiting for the new client's first slot 267" };
            printf("  operator restart window : %s%s\n", fl <= 2 ? wn[fl] : "?", (fl == 0 && arg && strtoull(arg, NULL, 0) == 0) ? "  (it was not opened)" : "");
            if (fl) printf("  time left               : %llu ms of 15000 - run `killall -9 WindowServer` now\n", (unsigned long long)out[5]);
            printf("  opened / closes tolerated / closed by slot 267 / expired: %llu / %llu / %llu / %llu\n", (unsigned long long)out[6], (unsigned long long)out[7], (unsigned long long)out[8], (unsigned long long)out[9]);
            printf("  pipe / GPU              : %s / %s\n", out[10] ? "ARMED" : "not armed (the window tolerates nothing)", out[11] ? "HUNG (the window is NOT honoured; HUNG disarms)" : "ok");
        } else if (in == 86) {
            printf("  event machine           : %#llx (SUSPECTED layout: em+0x30 count, +0xf8 completed, +0xfc submitted; no per-stamp array is named by the note)\n", fl);
            printf("  stamp count raw/clamped : %llu / %llu\n", (unsigned long long)out[5], (unsigned long long)out[6]);
            printf("  completed / submitted   : %llu / %llu%s\n", (unsigned long long)out[7], (unsigned long long)out[8], out[9] ? "   HAZARD: submitted is ahead (a mode change would block)" : "");
        } else {
            printf("  shortcut switch         : %llu (1 = ON, the default)\n", fl);
            printf("  shortcut applied / refused by the flag guard: %llu / %llu; slot 62 calls %llu, handled %llu, not handled %llu\n", (unsigned long long)out[5], (unsigned long long)out[6],
                   (unsigned long long)out[7], (unsigned long long)out[8], (unsigned long long)out[9]);
        }
        if (st != 0 && !(in == 83 && st == 11)) return 3;
    }
    if (in == 88) {
        // 0.0.614: out[3 + i] = v[i] of navi48_agdc_native_control (DisplayPipeGuard.cpp agdc_fill_out); v[0] is the n48agdc::Status.
        static const char *sn[] = { "PUBLISHED", "display is OFF (boot-arg navi48-metal-disp is not 1)", "(unused)", "AppleGraphicsDeviceControl class is not loaded",
                                    "the two slide anchors disagree (or the slide is not page aligned)", "AGDC class size is not 0x110", "a function we call does not carry its expected first bytes",
                                    "AGDC vtable slots 7/184/238/267 are not the expected functions", "no RDNA4FB / PCI device / provider", "no AppleGPUWrangler (AGDC::start would wait for it)",
                                    "allocation failed", "AGDC::start returned false", "already published", "bad argument", "row-120 mode hold is up (the link timing would not be the framebuffer's)",
                                    "IOAccelDisplayPipe class (the second slide anchor) is not loaded" };
        const unsigned long long st = out[3];
        printf("  agdc status             : %llu (%s)\n", st, st <= 15 ? sn[st] : "?");
        printf("  published / start ok    : %llu / %llu\n", out[4] & 1, (out[4] >> 1) & 1);
        printf("  registry ID             : %#llx\n", out[5]);
        printf("  vendor calls            : %llu (vendor info %llu, GPU capability %llu, other %llu, bad length %llu)\n", out[6],
               out[7] & 0xffffffffull, out[7] >> 32, out[8] & 0xffffffffull, out[8] >> 32);
        for (unsigned i = 9; i < 16; i++)
            if (out[i]) printf("  command %#llx x%llu\n", out[i] >> 32, out[i] & 0xffffffffull);
        if (st != 0 && st != 12) return 3;
    }
    if (in == 78) {
        // 0.0.308: out[3 + i] = v[i] of n48fbname::control.
        char nm[9]; unsigned long long w = out[9];
        for (int i = 0; i < 8; i++) nm[i] = (char)((w >> (8 * i)) & 0xff);
        nm[8] = 0;
        printf("  armed                   : %llu (status %llu)\n", (unsigned long long)out[3],
               (unsigned long long)out[4]);
        printf("  className word index    : %lld  (-1 = not located)\n", (long long)(int64_t)out[5]);
        printf("  arms / disarms / refusals: %llu / %llu / %llu\n", (unsigned long long)out[6],
               (unsigned long long)out[7], (unsigned long long)out[8]);
        printf("  class name NOW          : %s\n", nm);
        printf("  OSMetaClass             : %#llx\n", (unsigned long long)out[10]);
    }
    if (in == 77) {
        // 0.0.302: out[3 + i] = v[i] of n48dcn::mode.
        printf("  display layer armed     : %llu (status %llu)\n", (unsigned long long)out[3],
               (unsigned long long)out[4]);
        printf("  lit OTG                 : %lld\n", (long long)(int64_t)out[5]);
        printf("  DWORDS DIFFERING after  : %llu   (0 = the write-back changed nothing)\n",
               (unsigned long long)out[6]);
        printf("  timing before           : %llux%llu active, h_total %llu v_total %llu\n",
               out[7] & 0xffffULL, (out[7] >> 16) & 0xffffULL, (out[7] >> 32) & 0xffffULL,
               (out[7] >> 48) & 0xffffULL);
        printf("  timing after            : %llux%llu active, h_total %llu v_total %llu\n",
               out[8] & 0xffffULL, (out[8] >> 16) & 0xffffULL, (out[8] >> 32) & 0xffffULL,
               (out[8] >> 48) & 0xffffULL);
        printf("  frame count %llu -> %llu (advanced %llu; 0 would mean the OTG stopped)\n",
               (unsigned long long)out[9], (unsigned long long)out[10], (unsigned long long)out[11]);
        printf("  allowlist allowed/refused: %llu / %llu\n", (unsigned long long)out[12],
               (unsigned long long)out[13]);
        printf("  stalls / re-timings / golden restores: %llu / %llu / %llu\n", out[14] & 0xffffULL,
               (out[14] >> 16) & 0xffffULL, (out[14] >> 32) & 0xffffULL);
        printf("  REAL CHANGE fired       : %llu   (0 = same-mode re-timing only)\n",
               (out[15] >> 62) & 1ULL);
        printf("  refresh before/with/after: %llu / %llu / %llu mHz\n", out[15] & 0xfffffULL,
               (out[15] >> 20) & 0xfffffULL, (out[15] >> 40) & 0xfffffULL);
        printf("  lock held before / master_en after : %llu / %llu\n", (out[15] >> 60) & 1ULL,
               (out[15] >> 61) & 1ULL);
    }
    if (in == 76) {
        // 0.0.301 (T3-FLIP): out[3 + i] = v[i] of n48dcn::flip.
        static const char *w[] = {"flip-pending-on-write","flip-pending-cleared","earliest-inuse==test",
                                  "plane-address-RESTORED","CRC-changed","CRC-returned","console-CRC-stable"};
        printf("  display layer armed     : %llu (status %llu)\n", (unsigned long long)out[3],
               (unsigned long long)out[4]);
        printf("  OTG / HUBP              : %llu / %llu\n", out[5] & 0xffULL, (out[5] >> 8) & 0xffULL);
        printf("  console plane / test    : %#llx / %#llx\n", (unsigned long long)out[6],
               (unsigned long long)out[7]);
        printf("  plane geometry          : %llux%llu pitch %llu px, pixel format %llu\n",
               out[14] & 0xffffULL, (out[14] >> 16) & 0xffffULL, (out[14] >> 32) & 0xffffULL,
               (out[14] >> 48) & 0xffffULL);
        printf("  CRC console/test/restored: %#llx / %#llx / %#llx\n", (unsigned long long)out[9],
               (unsigned long long)out[10], (unsigned long long)out[11]);
        printf("  witness %#llx :", (unsigned long long)out[8]);
        for (int i = 0; i < 7; i++) printf(" %s=%llu", w[i], (out[8] >> i) & 1ULL);
        printf("\n");
        printf("  allowlist allowed/refused: %llu / %llu\n", (unsigned long long)out[12],
               (unsigned long long)out[13]);
        printf("  plane address NOW       : %#llx   (must equal the console plane above)\n",
               (unsigned long long)out[15]);
    }
    if (in == 74 || in == 75) {
        // 0.0.300 (Track D stage 1): out[3 + i] = v[i] of n48dcn::state / n48dcn::vbl.
        printf("  display layer armed     : %llu (init status %llu)\n", (unsigned long long)out[3],
               (unsigned long long)out[4]);
        if (in == 74) {
            printf("  lit OTG                 : %lld\n", (long long)(int64_t)out[5]);
            printf("  frame count             : %llu\n", (unsigned long long)out[6]);
            printf("  OTG_GLOBAL_SYNC_STATUS  : %#010llx\n", (unsigned long long)out[7]);
            printf("  DCE IH entries          : %llu  (VSTARTUP %llu, VUPDATE_NO_LOCK %llu)\n",
                   (unsigned long long)out[8], (unsigned long long)out[9], (unsigned long long)out[10]);
            printf("  entry span              : %llu ns\n", (unsigned long long)out[11]);
            printf("  allowlist allowed/refused: %llu / %llu\n", (unsigned long long)out[12],
                   (unsigned long long)out[13]);
            printf("  source enabled/kind/inst: %llu / %llu / %llu\n", (out[14] >> 8) & 1ULL,
                   (out[14] >> 4) & 0xfULL, out[14] & 0xfULL);
            printf("  selftest bits (1 legal ALLOWED, 2/4/8 would be BUGS): %#llx\n",
                   (unsigned long long)out[15]);
        } else {
            printf("  irq_set return          : %lld (0 = DCN41_OK)\n", (long long)(int64_t)out[4]);
            printf("  lit OTG                 : %lld\n", (long long)(int64_t)out[5]);
            printf("  frame count             : %llu\n", (unsigned long long)out[6]);
            printf("  GLOBAL_SYNC_STATUS      : %#010llx -> %#010llx\n", (unsigned long long)out[7],
                   (unsigned long long)out[8]);
            printf("  DCE entries total/at-enable: %llu / %llu\n", (unsigned long long)out[9],
                   (unsigned long long)out[10]);
            printf("  allowlist allowed/refused: %llu / %llu (last refused %#010llx)\n",
                   (unsigned long long)out[11], (unsigned long long)out[12],
                   (unsigned long long)out[13]);
            printf("  source enabled/kind/inst: %llu / %llu / %llu\n", (out[14] >> 8) & 1ULL,
                   (out[14] >> 4) & 0xfULL, out[14] & 0xfULL);
            printf("  enabled at              : %llu ns\n", (unsigned long long)out[15]);
        }
    }
    if (in == 65) {
        // 0.0.283 : out[3 + i] = v[i] of hw_hook_gfx_probe.
        printf("  probe armed / considered: %llu / %llu\n", (unsigned long long)out[3], (unsigned long long)out[4]);
        printf("  drawables FILLED        : %llu (pages %llu, bytes %llu)\n", (unsigned long long)out[5], (unsigned long long)out[11],
               (unsigned long long)out[12]);
        printf("  refused plan/page/window: %llu / %llu / %llu; write failures %llu; read-back bad %llu\n", (unsigned long long)out[6],
               (unsigned long long)out[7], (unsigned long long)out[8], (unsigned long long)out[9], (unsigned long long)out[10]);
        printf("  not SecurityAgent       : %llu (last VA and size in the driver log)\n", (unsigned long long)out[13]);
    }
    if (in == 64) {
        // 0.0.282 : out[3 + i] = v[i] of hw_hook_gfx_capture.
        printf("  capture armed           : %llu\n", (unsigned long long)out[3]);
        printf("  submissions seen / full : %llu / %llu\n", (unsigned long long)out[4], (unsigned long long)out[5]);
        printf("  IBs / regions / refs    : %llu / %llu / %llu\n", (unsigned long long)out[6], (unsigned long long)out[7], (unsigned long long)out[8]);
        printf("  programs                : %llu\n", (unsigned long long)out[9]);
        printf("  CONTEXT2 disagreed      : %llu; no reader %llu; busy %llu; append failures %llu\n", (unsigned long long)out[10],
               (unsigned long long)out[11], (unsigned long long)out[12], (unsigned long long)out[13]);
        printf("  ring dropped / records  : %llu / %llu\n", (unsigned long long)out[14], (unsigned long long)out[15]);
    }
    if (in == 63) {
        printf("  method hook / refusals  : %llu / %llu (entry refusals %llu)\n", (unsigned long long)(out[3] & 0xff), (unsigned long long)(out[3] >> 8),
               (unsigned long long)out[15]);
        printf("  set_surface / blit      : %llu / %llu\n", (unsigned long long)out[4], (unsigned long long)out[7]);
        printf("  finish calls / returns  : %llu / %llu (in flight %llu, longest %llu us)\n", (unsigned long long)out[5],
               (unsigned long long)out[6], (unsigned long long)out[8], (unsigned long long)out[11]);
        printf("  WindowServer finish     : %llu / %llu returned\n", (unsigned long long)out[9], (unsigned long long)out[10]);
        printf("  last finish arg / kr    : %#llx / %#llx; post-return surface lines %llu; contexts minted %llu patched %llu\n",
               (unsigned long long)(out[12] & 0xffffffffull), (unsigned long long)(out[12] >> 32), (unsigned long long)out[13],
               (unsigned long long)(out[14] & 0xffffffffull), (unsigned long long)(out[14] >> 32));
    }
    if (in == 61) {
        printf("  census armed / frames   : %llu / %llu (detailed %llu)\n", (unsigned long long)out[3], (unsigned long long)out[4],
               (unsigned long long)out[5]);
        printf("  IBs read / short / dw   : %llu / %llu / %llu; program heads %llu\n", (unsigned long long)out[6],
               (unsigned long long)out[7], (unsigned long long)out[8], (unsigned long long)out[9]);
        printf("  2D hook installed/mode  : %llu / %llu; contexts minted %llu patched %llu; blitCopy %llu blitFill %llu\n",
               (unsigned long long)out[10], (unsigned long long)out[11], (unsigned long long)out[12], (unsigned long long)out[13],
               (unsigned long long)out[14], (unsigned long long)out[15]);
    }
    if (in == 59 && arg && strtoull(arg, NULL, 0) == 4) {
        // 0.0.275 : `flushhook 4` - the rejection counters (a different layout; the mode is unchanged)
        printf("  selector 7 refused      : %llu (id<256 %llu, bad bits %llu, front+back %llu, no windowed %llu, other %llu)\n",
               (unsigned long long)out[4], (unsigned long long)out[5], (unsigned long long)out[6], (unsigned long long)out[7],
               (unsigned long long)out[8], (unsigned long long)out[9]);
        printf("  shape refused           : %llu (option 0x4000 %llu, mode 0x400/0x800 %llu, fb index %llu, no windowed %u, other %u)\n",
               (unsigned long long)out[10], (unsigned long long)out[11], (unsigned long long)out[12], (unsigned long long)out[13],
               (unsigned)(out[14] & 0xffffffffu), (unsigned)(out[14] >> 32));
        printf("  WindowServer refused/ok : %u / %u (selector 7 creates OK from WindowServer)\n",
               (unsigned)(out[15] >> 32), (unsigned)(out[15] & 0xffffffffu));
        return 0;
    }
    if (in == 59) {
        // 0.0.272 extras (v[i] == out[3 + i]), navi48_flushhook_control: 0 status, 1 mode, 2 installed, 3 slide,
        // 4 minted | patched<<32, 5 foreign | guard refused<<32, 6 sel7 | non-zero<<32, 7 shape, 8 lock | unlock<<32,
        // 9 flush | non-zero<<32, 10 copies ok | refused<<32, 11 last copy status | plan<<32, 12 last id | fbs<<32
        static const char *stn[] = { "ok", "REFUSED: no accelerator vtable copy (fire first)", "REFUSED: slide",
                                     "REFUSED: accelerator slot 326 is not newSurface", "copy REFUSED: scanout positive control not passed",
                                     "REFUSED: bad argument" };
        const unsigned long long st = out[3];
        printf("  flushhook status        : %llu (%s)\n", st, st <= 5 ? stn[st] : "?");
        printf("  mode / installed / slide: %llu (%s) / %llu / 0x%llx\n", (unsigned long long)out[4],
               out[4] == 3 ? "log + COPY" : out[4] == 1 ? "LOG-ONLY" : "pass-through", (unsigned long long)out[5],
               (unsigned long long)out[6]);
        printf("  surfaces minted/patched : %u / %u\n", (unsigned)(out[7] & 0xffffffffu), (unsigned)(out[7] >> 32));
        printf("  foreign / guard refused : %u / %u\n", (unsigned)(out[8] & 0xffffffffu), (unsigned)(out[8] >> 32));
        printf("  selector 7 (non-zero kr): %u (%u)\n", (unsigned)(out[9] & 0xffffffffu), (unsigned)(out[9] >> 32));
        printf("  shape calls             : %llu\n", (unsigned long long)out[10]);
        printf("  lock / unlock calls     : %u / %u\n", (unsigned)(out[11] & 0xffffffffu), (unsigned)(out[11] >> 32));
        printf("  flush calls (non-zero)  : %u (%u)\n", (unsigned)(out[12] & 0xffffffffu), (unsigned)(out[12] >> 32));
        printf("  copies ok / refused     : %u / %u\n", (unsigned)(out[13] & 0xffffffffu), (unsigned)(out[13] >> 32));
        printf("  last copy status / plan : %u / %u\n", (unsigned)(out[14] & 0xffffffffu), (unsigned)(out[14] >> 32));
        printf("  last flushed id / fbs   : 0x%x / %u\n", (unsigned)(out[15] & 0xffffffffu), (unsigned)(out[15] >> 32));
        if (st != 0) return 3;
    }
    if (in == 60) {
        // 0.0.272 extras, navi48_scanout_control: 0 status, 1 geometry reason | plan reason<<8, 2 scanout VRAM offset,
        // 3 rect x<<48|y<<32|w<<16|h, 4 pre-flight mismatches, 5 pre-flight us<<32 | scanout us, 6 rect mismatches,
        // 7 first mismatch index<<32 | value, 8..11 samples expected<<32|read, 12 passed | runs<<8 | copies<<24 | refusals<<40
        static const char *stn[] = { "OK", "no bring-up context", "no RDNA4FB / Console properties", "geometry REFUSED",
                                     "MC base disagrees", "SDMA0 QUEUE0 not up", "SDMA0 QUEUE0 BUSY", "VRAM allocation failed",
                                     "source control FAILED", "PRE-FLIGHT copy FAILED", "FENCE timeout", "READBACK mismatch",
                                     "plan refused", "interlock: positive control not passed", "ring write failed",
                                     "nothing saved to restore", "buffer unresolved", "backing unprepared or short",
                                     /* 0.0.416 mode 7 (notes/design/SDMA-GCR.md G2) */
                                     "mode 7 source not 4 KiB aligned", "mode 7 source outside the card's VRAM",
                                     "mode 7 source overlaps one of our own allocations",
                                     /* 0.0.518 / 0.0.542 */
                                     "flip mode owns A/B (switch 74 ON)", "scanout full REFUSED (reason below)" };
        const unsigned long long st = out[3];
        printf("  scanout status          : %llu (%s)\n", st, st < sizeof stn / sizeof stn[0] ? stn[st] : "?");
        if (in2[1] == N48_SF_MODE || in2[1] == N48_SF_MODE_RELEASE) {
            /* build 0.0.542 (apple/scanout_full.h): out[3 + i] = v[i]: 1 reason, 2 surface MC, 3 payload bytes, 4 w<<32|h,
               5 pitch<<32|fmt<<8|SW_MODE, 6 which|flags<<8, 7 copy us, 8 chunks, 9 FNV-1a, 10 capture #, 11 frames, 12 total bytes. */
            printf("  scanout full reason     : %llu (%s)\n", (unsigned long long)out[4], n48_sf_reason_name((uint32_t)out[4]));
            if (in2[1] == N48_SF_MODE_RELEASE) return st != 0 ? 3 : 0;
            printf("  surface MC / which      : 0x%llx / %s (flags 0x%llx)\n", (unsigned long long)out[5],
                   (out[9] & 0xff) == 1 ? "A (console)" : (out[9] & 0xff) == 2 ? "B (flip mode)" : "-", (unsigned long long)(out[9] >> 8));
            printf("  geometry                : %llux%llu pitch %llu fmt %llu SW_MODE %llu; %llu payload bytes\n",
                   (unsigned long long)(out[7] >> 32), (unsigned long long)(out[7] & 0xffffffffu), (unsigned long long)(out[8] >> 32),
                   (unsigned long long)((out[8] >> 8) & 0xff), (unsigned long long)(out[8] & 0xff), (unsigned long long)out[6]);
            if (st != 0) return 3;
            printf("  copy                    : %llu us, %llu chunk(s), FNV-1a %08llx, capture #%llu, OTG0 frames %llu -> %llu\n",
                   (unsigned long long)out[10], (unsigned long long)out[11], (unsigned long long)out[12], (unsigned long long)out[13],
                   (unsigned long long)(out[14] >> 32), (unsigned long long)(out[14] & 0xffffffffu));
            return scanfull_pull(arg2, out[15], out[13]);
        }
        if (arg && strtoull(arg, NULL, 0) == 4) {
            // 0.0.281 : the read-only THUMBNAIL; the cells themselves are in the driver log (scanout-thumb: lines)
            printf("  thumbnail cols x rows   : %llu x %llu (cells in the driver log, `scanout-thumb:` lines)\n", (unsigned long long)out[4],
                   (unsigned long long)out[5]);
            printf("  thumbnail FNV-1a        : 0x%016llx\n", (unsigned long long)out[6]);
            printf("  lit cells / max luma    : %llu / %llu; mean luma x1000 %llu\n", (unsigned long long)out[7], (unsigned long long)out[8],
                   (unsigned long long)out[9]);
            return st != 0 ? 3 : 0;
        }
        if (arg && strtoull(arg, NULL, 0) == 5) {
            /* 0.0.345 : the UNARMED DETILE PROOF. Three scratch VRAM buffers only - it never
               touches the scanout, needs no interlock, no WindowServer and no Apple accelerator, and costs neither
               a reboot nor a panic. The four measurements are in the driver log as `scanout-tiled:` lines. */
            printf("  surface / tiled VRAM    : %ux%u at 0x%llx (swizzle 3, ADDR3_64KB_2D, 32 bpp)\n",
                   (unsigned)(out[6] >> 16), (unsigned)(out[6] & 0xffff), (unsigned long long)out[5]);
            printf("  DETILE MM WINDOW wrong  : %llu of %u  <== THE VERDICT (0 means the packet detiles correctly)\n",
                   (unsigned long long)(out[4] & 0xffffffffull),
                   (unsigned)(out[6] >> 16) * (unsigned)(out[6] & 0xffff));
            printf("  detile BAR0 wrong       : %llu (second opinion)\n", (unsigned long long)(out[4] >> 32));
            printf("  TILE BACK wrong         : %llu  <== the hardware's WRITE side IS the addrlib equation when 0\n",
                   (unsigned long long)(out[8] & 0xffffffffull));
            printf("  SUB-WINDOW wrong        : %llu of 16384 (asymmetric field check at tiled 128,128)\n",
                   (unsigned long long)(out[8] >> 32));
            printf("  DISCRIMINATOR wrong     : %llu of 65536  <== same packet, but the tiled source is the one SDMA wrote\n",
                   (unsigned long long)out[11]);
            printf("  wrote past the rect     : %llu of 4 sampled (must be 0)\n", (unsigned long long)out[13]);
            /* 0.0.346 : the controls whose absence would make every number above meaningless. */
            printf("  CONTROLS before the copy: source wrong MM/BAR0 %llu / %llu of 65536, destination poison NOT landed %llu\n",
                   (unsigned long long)(out[12] & 0xffffull), (unsigned long long)((out[12] >> 16) & 0xffffull),
                   (unsigned long long)(out[12] >> 32));
            printf("  differs from linear     : %llu position(s)  <== the negative control; 0 would void TILE BACK\n",
                   (unsigned long long)out[10]);
            printf("  fence us detile / tile  : %u / %u\n", (unsigned)(out[7] & 0xffffffffu), (unsigned)(out[7] >> 32));
            printf("  first mismatch          : index %u read 0x%08x (decoded in the `scanout-tiled: DETILE first` line)\n",
                   (unsigned)(out[9] >> 32), (unsigned)(out[9] & 0xffffffffu));
            return st != 0 ? 3 : 0;
        }
        if (arg && strtoull(arg, NULL, 0) == 6) {
            /* 0.0.414 (notes/design/SCANOUT-SELFTEST-FULL.md, section 950): the FULL-GEOMETRY SDMA self-test.
               One call, two cases in order: the 256x256 section 719 control, then the live 1920x1080 geometry.
               Three scratch VRAM buffers only - it never touches the scanout, needs no interlock, no WindowServer
               and no Apple accelerator. The 16x16 wrong-cell maps and the decoded wrong pixels are in the driver
               log as `scanout-full:` lines (every line under the logger's 512 bytes). out: 3 status,
               4 case0 tile|detile wrong, 5 case0 poison|BAR0-vs-MM, 6 case0 tile|detile sampled,
               7 case1 tile|detile wrong, 8 case1 poison|BAR0-vs-MM, 9 case1 tile|detile sampled,
               10 case0 tile|detile status, 11 case1 tile|detile status. */
            printf("  full self-test          : 256x256 control then 1920x1080 live (maps/decodes in the driver log)\n");
            printf("  256x256 control         : tile wrong %u/%u, detile wrong %u/%u, poison %u, BAR0-vs-MM %u; SDMA status tile/detile %u/%u\n",
                   (unsigned)out[4], (unsigned)(out[6] & 0xffffffffu), (unsigned)(out[4] >> 32), (unsigned)(out[6] >> 32),
                   (unsigned)out[5], (unsigned)(out[5] >> 32), (unsigned)(out[10] >> 32), (unsigned)out[10]);
            printf("  1920x1080 live          : tile wrong %u/%u, detile wrong %u/%u, poison %u, BAR0-vs-MM %u; SDMA status tile/detile %u/%u\n",
                   (unsigned)out[7], (unsigned)(out[9] & 0xffffffffu), (unsigned)(out[7] >> 32), (unsigned)(out[9] >> 32),
                   (unsigned)out[8], (unsigned)(out[8] >> 32), (unsigned)(out[11] >> 32), (unsigned)out[11]);
            printf("  the control must be 0/0; a non-zero live row is the MEASUREMENT (section 950), not a refusal\n");
            return st != 0 ? 3 : 0;
        }
        if (arg && strtoull(arg, NULL, 0) == 7) {
            /* 0.0.416 (notes/design/SDMA-GCR.md, section 953): the SDMA CACHE-RINSE INSTRUMENT. READ-ONLY on the
               source. ONE COPY_LINEAR of a 256 KiB window, optionally with the GCR_REQ (GL2 write-back + invalidate)
               immediately before it in the SAME submission, into a low scratch of ours; then the scratch and the
               source are both read through the MM window and compared per 256-byte line.
               CLI: `accel scanout 7 <vramOff> [gcr]`; `accel scanout 7 0` is the CONTROL (a low pattern buffer).
               The first 8 differing dwords (offset, MM source, SDMA scratch) are in the driver log (`scanout-gcr:`).
               out: 3 status, 4 gcr|control<<1, 5 source VRAM offset, 6 scratch VRAM offset,
               7 fence us | copy dwords<<32, 8 lines full|part<<16|fully-differing<<32, 9 differing dwords,
               10 the 64-page difference map. */
            const unsigned sfull = (unsigned)(out[8] & 0xffffu), spart = (unsigned)((out[8] >> 16) & 0xffffu),
                           sdiff = (unsigned)(out[8] >> 32);
            printf("  cache-rinse mode        : %s, GCR_REQ %s; source VRAM %#llx -> scratch VRAM %#llx\n",
                   (out[4] & 2) ? "CONTROL (low pattern buffer)" : "PLANE", (out[4] & 1) ? "ON" : "off",
                   (unsigned long long)out[5], (unsigned long long)out[6]);
            printf("  copy                    : COPY_LINEAR+GCR = %u dword(s), fence %u us\n",
                   (unsigned)(out[7] >> 32), (unsigned)out[7]);
            printf("  256 KiB lines           : %u full, %u partly, %u fully differing of %u; differing dwords %u\n",
                   sfull, spart, sdiff, sfull + spart + sdiff, (unsigned)out[9]);
            printf("  pages with a difference : map 0x%016llx (bit N = 4-KiB page N)\n", (unsigned long long)out[10]);
            printf("  the CONTROL must read 0 differing dwords; a non-zero PLANE row is the MEASUREMENT (section 953)\n");
            return st != 0 ? 3 : 0;
        }
        if (arg && strtoull(arg, NULL, 0) == 8) {
            /* D7 (0.0.417, notes/design/SDMA-DCC-NOPTE.md): THE UNIFORM PROBE. Mode 6's live 1920x1080 case
               exactly - same buffers, packet fields, sample set, report lines and POISON detection - except the
               CPU writes 0xff00ff00 everywhere and every expected value is 0xff00ff00. It measures the WRITE
               compression half: with SDMA0's no-PTE write compression ON a constant block is stored as a code
               the raw MM window reads back, so detile wrong > 0 is expected; after `sdmadcc 1` it must be 0.
               out: 3 status, 7 tile|detile wrong, 8 poison|BAR0-vs-MM, 9 tile|detile sampled, 11 tile|detile
               status. The `scanout-full:` lines in the driver log carry the map and the decode. */
            printf("  full self-test          : 1920x1080 UNIFORM probe 0x%08x (maps/decodes in the driver log)\n",
                   (unsigned)N48_TILE_UNIFORM_PIXEL);
            printf("  1920x1080 uniform       : tile wrong %u/%u, detile wrong %u/%u, poison %u, BAR0-vs-MM %u; SDMA status tile/detile %u/%u\n",
                   (unsigned)out[7], (unsigned)(out[9] & 0xffffffffu), (unsigned)(out[7] >> 32), (unsigned)(out[9] >> 32),
                   (unsigned)out[8], (unsigned)(out[8] >> 32), (unsigned)(out[11] >> 32), (unsigned)out[11]);
            printf("  with SDMA no-PTE compression ON a non-zero detile count is the MEASUREMENT; `sdmadcc 1` then `scanout 8` must read 0\n");
            return st != 0 ? 3 : 0;
        }
        if (arg && strtoull(arg, NULL, 0) == 3) {
            // 0.0.279 : the read-only CONTENT read - a different layout
            printf("  control rect: bars kept : %llu of %llu (%llu differ)\n", (unsigned long long)out[4], (unsigned long long)out[5],
                   (unsigned long long)out[15]);
            printf("  sampled FNV-1a          : 0x%016llx\n", (unsigned long long)out[6]);
            printf("  sampled non-black/changes/samples: %llu / %llu / %llu\n", (unsigned long long)out[7], (unsigned long long)out[8],
                   (unsigned long long)out[9]);
            printf("  centre / TL TR BL BR    : 0x%08x / 0x%08x 0x%08x 0x%08x 0x%08x\n", (unsigned)out[10], (unsigned)out[11],
                   (unsigned)out[12], (unsigned)out[13], (unsigned)out[14]);
            return st != 0 ? 3 : 0;
        }
        printf("  geometry / plan reason  : %llu / %llu\n", (unsigned long long)(out[4] & 0xff), (unsigned long long)((out[4] >> 8) & 0xff));
        /* 0.0.337 : the pre-flight compare is now four readings of the same bytes, and the MM
           window - the GPU's own view of VRAM - is the verdict. Every BAR0 number is still printed. */
        printf("  PRE-FLIGHT MM WINDOW    : %llu of 16384 wrong  <== THE VERDICT (0 means the SDMA copy is correct)\n",
               (unsigned long long)(out[7] >> 32));
        printf("  pre-flight BAR0 1/2/3   : %llu / %llu / %llu (pass 3 is after amdgpu_hdp_flush; poison 0xdeadbeef %llu)\n",
               (unsigned long long)(out[7] & 0xffffffffull), (unsigned long long)((out[4] >> 16) & 0xffff),
               (unsigned long long)((out[4] >> 32) & 0xffff), (unsigned long long)((out[4] >> 48) & 0xffff));
        if (st == 9)
            printf("  pre-flight 1st mismatch : index %u read 0x%08x (the kext log's `PRE-FLIGHT first` line has six)\n",
                   (unsigned)(out[10] >> 32), (unsigned)(out[10] & 0xffffffffu));
        printf("  scanout VRAM offset     : 0x%llx\n", (unsigned long long)out[5]);
        printf("  rectangle x,y w x h     : %u,%u %ux%u\n", (unsigned)(out[6] >> 48), (unsigned)((out[6] >> 32) & 0xffff),
               (unsigned)((out[6] >> 16) & 0xffff), (unsigned)(out[6] & 0xffff));
        printf("  fence us pre-flight/copy: %u / %u\n", (unsigned)(out[8] >> 32), (unsigned)(out[8] & 0xffffffffu));
        printf("  rect MM WINDOW wrong    : %u of 16384  <== THE VERDICT for the scanout rectangle\n", (unsigned)(out[9] >> 32));
        printf("  rectangle mismatches    : %llu (first index %u value 0x%08x)\n", (unsigned long long)(out[9] & 0xffffffffull),
               (unsigned)(out[10] >> 32), (unsigned)(out[10] & 0xffffffffu));
        printf("  samples expected/read   : 0x%08x/0x%08x 0x%08x/0x%08x 0x%08x/0x%08x 0x%08x/0x%08x\n",
               (unsigned)(out[11] >> 32), (unsigned)out[11], (unsigned)(out[12] >> 32), (unsigned)out[12],
               (unsigned)(out[13] >> 32), (unsigned)out[13], (unsigned)(out[14] >> 32), (unsigned)out[14]);
        printf("  interlock / runs / copies / refusals: %s / %u / %u / %u\n", (out[15] & 1) ? "PASSED" : "not passed",
               (unsigned)((out[15] >> 8) & 0xffff), (unsigned)((out[15] >> 24) & 0xffff), (unsigned)((out[15] >> 40) & 0xffff));
        if (st != 0) return 3;
    }
    if (in == 82) {
        /* D1 (0.0.417, notes/design/SDMA-DCC-NOPTE.md): out[i] = v[i] of navi48_sdmadcc_control.
           v: 0 op (0 refused, 1 read, 2 set, 3 restore), 1 SDMA0 raw, 2 SDMA1 raw, 3 restore value,
           4 written/target, 5 read-back, 6 match, 7 status (0 ok, 1 bad arg, 2 no context, 3 no GC base,
           4 mismatch), 8 captured. The per-set decode uses sdma_dcc.h's own accessors. */
        static const char *opn[] = { "REFUSED", "READ", "SET", "RESTORE" };
        static const char *stn[] = { "OK", "bad argument (or 2 before a capture)", "no bring-up context",
                                     "GC BASE_IDX 0 unresolved", "read-back MISMATCH" };
        const unsigned long long op = out[3], s0 = out[4], s1 = out[5], st = out[10];
        printf("  sdmadcc op              : %llu (%s), captured %llu\n", op, op <= 3 ? opn[op] : "?",
               (unsigned long long)out[11]);
        printf("  status                  : %llu (%s)\n", st, st <= 4 ? stn[st] : "?");
        printf("  SDMA0_DCC_CNTL raw      : 0x%08x   SDMA1_DCC_CNTL raw (never written): 0x%08x\n",
               (unsigned)s0, (unsigned)s1);
        if (op == 2 || op == 3) {
            printf("  restore value           : 0x%08x\n", (unsigned)out[6]);
            printf("  wrote / read back       : 0x%08x / 0x%08x  %s\n", (unsigned)out[7], (unsigned)out[8],
                   out[9] ? "MATCH" : "MISMATCH");
        }
        if (op == 1) {
            printf("  force-bypass            : 0x%x\n", (unsigned)n48_sdma_dcc_force_bypass((uint32_t)s0));
            for (unsigned set = 0; set < 4u; set++)
                printf("  set%u                    : rd ovr %u comp %u, wr ovr %u comp %u\n", set,
                       (unsigned)n48_sdma_dcc_rd_override((uint32_t)s0, set), (unsigned)n48_sdma_dcc_rd_comp((uint32_t)s0, set),
                       (unsigned)n48_sdma_dcc_wr_override((uint32_t)s0, set), (unsigned)n48_sdma_dcc_wr_comp((uint32_t)s0, set));
        }
        if (st != 0) return 3;
    }
    if (in == 58) {
        // 0.0.269 extras (v[i] == out[3 + i]): 0 state | why<<8 | rings<<16 | mapped<<24, 1 submissions, 2 IBs,
        // 3 translated, 4 refused, 5 unreadable | overcap<<32, 6 write fails | verify bad<<32, 7 polls neutered,
        // 8 REG_WRITEs converted, 9 PTE leaves, 10 GPUVM_INV passed through, 11 max us, 12 ring stops | backwards<<32
        static const char *sn[] = { "OFF (boot-chain bit 2 not set)", "ARMING", "LIVE", "REFUSED" };
        const unsigned long long w = out[3];
        printf("  drain state             : %llu (%s), refusal reason %llu, rings %llu, mapped %llu\n",
               w & 0xff, (w & 0xff) <= 3 ? sn[w & 0xff] : "?", (w >> 8) & 0xff, (w >> 16) & 0xff, (w >> 24) & 0xff);
        printf("  submissions translated  : %llu\n", (unsigned long long)out[4]);
        printf("  IBs                     : %llu (translated %llu, refused %llu, unreadable %u, over cap %u)\n",
               (unsigned long long)out[5], (unsigned long long)out[6], (unsigned long long)out[7],
               (unsigned)(out[8] & 0xffffffffu), (unsigned)(out[8] >> 32));
        printf("  write fails / verify bad: %u / %u\n", (unsigned)(out[9] & 0xffffffffu), (unsigned)(out[9] >> 32));
        printf("  polls neutered          : %llu\n", (unsigned long long)out[10]);
        printf("  REG_WRITEs converted    : %llu\n", (unsigned long long)out[11]);
        printf("  PTEPDE leaves converted : %llu\n", (unsigned long long)out[12]);
        printf("  GPUVM_INV passed through: %llu\n", (unsigned long long)out[13]);
        printf("  max latency per call    : %llu us\n", (unsigned long long)out[14]);
        printf("  ring stops / backwards  : %u / %u\n", (unsigned)(out[15] & 0xffffffffu), (unsigned)(out[15] >> 32));
    }
    if (in == 50) {
        // 0.0.237 extras, positionally: 3 state, 4 mode, 5 failedAt, 6 syncPages,
        // 7 (syncTries<<32)|setvspace, 8 (pm4a<<32)|kiqenable, 9 (gfxmap<<32)|pm4b,
        // 10 copyarm, 11 chainMs, 12 takeoverState, 13 xlatregs,
        // 14 (neuterpoll<<32)|sdmamap, 15 takeoverMs.
        static const char *sn[] = { "idle (not armed)", "RUNNING", "DONE", "FAILED" };
        static const char *step[] = { "-", "synctables", "setvspace", "pm4powerup#1",
                                      "kiqenable", "gfxmap", "pm4powerup#2", "copyarm" };
        static const char *tk[] = { "not fired", "RUNNING", "DONE", "FAILED" };
        const unsigned long long st = out[3], fa = out[5], ts = out[12];
        printf("  bootchain state         : %llu (%s)\n", st, st <= 3 ? sn[st] : "?");
        printf("  mode (navi48-boot-chain): 0x%llx  (bit0 accel-start chain, bit1 submission takeover)\n",
               (unsigned long long)out[4]);
        if (fa) printf("  FAILED AT               : step %llu (%s)\n", fa, fa <= 7 ? step[fa] : "?");
        printf("  synctables              : %llu page(s) in %u try/tries\n",
               (unsigned long long)out[6], (unsigned)(out[7] >> 32));
        printf("  setvspace / kiqenable   : %u / %u\n",
               (unsigned)(out[7] & 0xffffffffu), (unsigned)(out[8] & 0xffffffffu));
        printf("  pm4powerup #1 / #2      : %u / %u\n",
               (unsigned)(out[8] >> 32), (unsigned)(out[9] & 0xffffffffu));
        printf("  gfxmap / copyarm        : %u / 0x%llx\n",
               (unsigned)(out[9] >> 32), (unsigned long long)out[10]);
        printf("  phase A wall clock      : %llu ms\n", (unsigned long long)out[11]);
        printf("  phase B takeover        : %llu (%s)\n", ts, ts <= 3 ? tk[ts] : "?");
        printf("    xlatregs / neuterpoll / sdmamap : %llu / %u / %u   in %llu ms\n",
               (unsigned long long)out[13], (unsigned)(out[14] >> 32),
               (unsigned)(out[14] & 0xffffffffu), (unsigned long long)out[15]);
    }
    if (in == 52) {
        // r80 DEFECT, fixed: the kext's verb-specific scalar v[i] arrives at out[3 + i]
        // (Navi48UserClient.cpp: scalarOutput[3 + i] = extra[i]), so out[3] is v[0], the
        // STATUS - not v[1]. The first version of this block read out[3] as the root and
        // printed every field one place early: a status of 2 shown as "live CONTEXT2 root
        // 0x2", the root shown as the entry count, and 511 free slots shown as "distinct
        // (ctx, root) 511". The driver log was right throughout; only this display was
        // wrong. v[i] == out[3 + i]: 0 status, 1 live root, 2 valid entries, 3 bitmask,
        // 4 highest all-zero index, 5 distinct pairs, 6/7 first two roots, 8/9 arena
        // bottom/top, 10 free-tail base, 11 free-tail size, 12 (base writes << 32) | tables.
        printf("  live CONTEXT2 root      : 0x%llx\n", (unsigned long long)out[4]);
        printf("  VALID root entries      : %llu   (low-64 index bitmask 0x%llx)\n",
               (unsigned long long)out[5], (unsigned long long)out[6]);
        printf("  highest ALL-ZERO index  : %llu   -> VA 0x%llx, the candidate mapping slot\n",
               (unsigned long long)out[7],
               (unsigned long long)(0x400000000ull + ((unsigned long long)out[7] << 28)));
        printf("  distinct (ctx, root)    : %llu%s\n", (unsigned long long)out[8],
               out[8] > 1 ? "   *** more than one root table this boot: the VM table is"
                            " PER CLIENT PROCESS ***" : "");
        if (out[9])  printf("    root #0               : 0x%llx\n", (unsigned long long)out[9]);
        if (out[10]) printf("    root #1               : 0x%llx\n", (unsigned long long)out[10]);
        printf("  Apple page-table arena  : [0x%llx..0x%llx)\n",
               (unsigned long long)out[11], (unsigned long long)out[12]);
        printf("  free tail below it      : 0x%llx + 0x%llx bytes\n",
               (unsigned long long)out[13], (unsigned long long)out[14]);
        printf("  base writes / tables    : %llu / %llu\n",
               (unsigned long long)(out[15] >> 32),
               (unsigned long long)(out[15] & 0xffffffffull));
    }
    if (in == 53) {
        // 0.0.244 extras, positionally. v[i] arrives at out[3 + i] (the r80 off-by-one):
        // 3 status, 4 refusal reason, 5 ring base, 6 ring bytes, 7 L1 block offset,
        // 8 leaves, 9 L1 entries written, 10 read-back mismatches, 11 (probes << 32) |
        // probe mismatches, 12 ring VA base, 13 root slot, 14 Apple's root slot entry
        // (READ, never written), 15 free-tail size.
        static const char *rst[] = { "REFUSED - nothing reserved, nothing written",
                                     "reserved and verified (read-only; the L1 block was NOT written)",
                                     "BUILT and VERIFIED",
                                     "built, but a check MISMATCHED" };
        const unsigned long long s = out[3];
        printf("  %-24s: %s\n", "ringmap", rst[s <= 3 ? s : 0]);
        if (out[4])
            printf("  refusal reason          : %llu\n", (unsigned long long)out[4]);
        printf("  reserved ring region    : vram+0x%llx + 0x%llx bytes\n",
               (unsigned long long)out[5], (unsigned long long)out[6]);
        printf("  kext-owned L1 block     : vram+0x%llx\n", (unsigned long long)out[7]);
        printf("  64 KiB leaf entries     : %llu\n", (unsigned long long)out[8]);
        printf("  L1 entries written      : %llu   read-back MISMATCHED %llu\n",
               (unsigned long long)out[9], (unsigned long long)out[10]);
        printf("  our own walker          : %llu probe(s), MISMATCHED %llu\n",
               (unsigned long long)(out[11] >> 32),
               (unsigned long long)(out[11] & 0xffffffffull));
        printf("  ring VA base            : 0x%llx   (root slot %llu)\n",
               (unsigned long long)out[12], (unsigned long long)out[13]);
        printf("  Apple root slot (READ)  : 0x%llx%s\n", (unsigned long long)out[14],
               (out[14] & 1) ? "   *** VALID - increment (iii)'s identity guard must refuse ***"
                             : "   (not valid - nothing of ours is live)");
        {
            /* 0.0.259: the kext's v[12] arrives here as out[15] (v[i] -> out[3+i]),
               and it now carries THREE fields, because ringmap had no free scalar and
               kAccelExtraScalars is a hard ceiling. freeSz is in 4 KiB PAGES here, not
               bytes — printing it as bytes would be a number contradicting itself.

               The census is the measurement that turns "is a stale entry of ours sitting
               in Apple's arena?" from an inference into a count taken BEFORE anything
               depends on it. "ours" can only be an entry naming a page inside this boot's
               reservation, which Apple cannot have written; "foreign" is reported and
               never touched. */
            const unsigned long long cOurs    = (out[15] >> 48) & 0xffffu;
            const unsigned long long cForeign = (out[15] >> 32) & 0xffffu;
            const unsigned long long pages    =  out[15] & 0xffffffffull;
            printf("  free tail               : %llu page(s) = 0x%llx bytes\n",
                   pages, pages << 12);
            printf("  ARENA CENSUS slot 511   : %llu OURS, %llu foreign%s\n",
                   cOurs, cForeign,
                   cOurs ? "   <- a stale entry of ours is present; the mapVA arm may ADOPT it"
                         : "   (no stale entry of ours in Apple's arena)");
            if (cForeign)
                printf("                            %llu FOREIGN entr(y/ies) - reported, NEVER touched."
                       " UNEXPLAINED, not yet a fault: memory is not zeroed at boot, so stale bytes"
                       " can set bit 0. The first few are decoded in `navi48test log`; look for"
                       " STRUCTURE (an address in a live pool, or a repeating value).\n", cForeign);
        }
        printf("\nNOTHING was written into Apple's page table. The reservation arithmetic,\n"
               "the L1 block, the arena census and our own walk are in `navi48test log`.\n");
    }
    if (in == 54) {
        // 0.0.247 extras, positionally. v[i] arrives at out[3 + i]:
        // 3 state bits, 4 creates, 5 releases, 6 live, 7/8 root NOW of the first two
        // live contexts, 9 Apple's CONTEXT2 root, 10 live roots agreeing with it,
        // 11 (ENG17 LO32 << 32) | HI32, 12 root[511] of the first live context,
        // 13 (non-zero-at-create << 32) | first root at create, 14 last root at
        // release, 15 (table overflow << 32) | entries used.
        const unsigned long long st = out[3];
        printf("  %-24s: %s%s\n", "observe pair (40/41)",
               (st & 1) ? "INSTALLED" : ((st & 2) ? "REFUSED by the geometry check"
                                                  : "NOT installed"),
               (st & 4) ? "   (the VM manager vtable was hooked)"
                        : "   *** the VM manager was NEVER hooked this boot ***");
        printf("  createVMContext calls   : %llu%s\n", (unsigned long long)out[4],
               out[4] ? "" : "   *** slot 40 NEVER FIRED - the review's falsifier ***");
        printf("  releaseVMContext calls  : %llu\n", (unsigned long long)out[5]);
        printf("  live contexts now       : %llu   (table entries used %llu, overflowed %llu)\n",
               (unsigned long long)out[6], (unsigned long long)(out[15] & 0xffffffffull),
               (unsigned long long)(out[15] >> 32));
        printf("  root NOW (ctx+0x98+0x20): 0x%llx / 0x%llx\n",
               (unsigned long long)out[7], (unsigned long long)out[8]);
        printf("  Apple CONTEXT2 root     : 0x%llx\n", (unsigned long long)out[9]);
        printf("  live roots AGREEING     : %llu%s\n", (unsigned long long)out[10],
               out[10] ? "   <- the object's own root IS the one Apple programmed"
                       : "   *** none agree - the review's other falsifier ***");
        printf("  root at create (first)  : 0x%llx   (contexts non-zero at create: %llu)\n",
               (unsigned long long)(out[13] & 0xffffffffull),
               (unsigned long long)(out[13] >> 32));
        printf("  last root at release    : 0x%llx\n", (unsigned long long)out[14]);
        printf("  root[511] (first live)  : 0x%llx%s\n", (unsigned long long)out[12],
               (out[12] & 1) ? "   *** VALID - increment (iii)'s guard G4 must refuse ***"
                             : "   (exactly zero - guard G4's precondition holds)");
        printf("  ENG17 ADDR_RANGE LO/HI  : 0x%08llx / 0x%08llx%s\n",
               (unsigned long long)(out[11] >> 32),
               (unsigned long long)(out[11] & 0xffffffffull),
               ((out[11] >> 32) == 0xFFFFFFFFull && (out[11] & 0xffffffffull) == 0x1Full)
                   ? "   (full range - a VMID-2 invalidate covers everything)"
                   : "   *** NOT the full range - review section 4.2 does not hold ***");
        printf("\nREAD-ONLY. Neither this verb nor the slot-40/41 hooks wrote anything:\n"
               "not Apple's page tables, not Apple's objects, not a register, not VRAM.\n"
               "The per-context detail is in `navi48test log`.\n");
    }
    if (in == 56) {
        // 0.0.261 extras, positionally. v[i] arrives at out[3 + i]:
        // 3 status (0 refused, 1 report only, 2 re-armed+verified, 3 mismatch),
        // 4 refusal reason, 5 root[511] AFTER the drain (instrument C), 6 the root page
        // we armed, 7 Apple's CONTEXT2 root, 8 (packets touching the root page << 32) |
        // POSITIVE CONTROL PTEPDEs at root[0], 9 (packets walked << 32) | IBs that
        // stopped on an unknown stride, 10 (no-destination << 32) | addressed elsewhere,
        // 11 read-back, 12 fault status.
        static const char *rast[] = {
            "REFUSED - NOTHING was written into Apple's page table",
            "REPORT ONLY - every condition holds, NOTHING was written",
            "*** RE-ARMED after the drain and verified ***",
            "*** RE-ARMED but a read-back MISMATCHED - treat this boot as void ***"
        };
        static const char *rwhy[] = {
            "none", "no context this boot's mapVA hook armed",
            "identity lost (the object is no longer the one we armed)",
            "VMID 2 is not programmed (CONTEXT2 root is zero)",
            "the context's own root moved",
            "CONTEXT2 names a different root than the one we armed",
            "root[511] could not be read through the MM window",
            "root[511] is NOT zero - it is not ours to overwrite",
            "our own entry would point outside this boot's reservation"
        };
        const unsigned long long s = out[3], g = out[4];
        printf("  %-24s: %s\n", "rearmdrain", rast[s <= 3 ? s : 0]);
        printf("  refusal reason          : %s\n", g <= 8 ? rwhy[g] : "UNKNOWN");
        printf("  root[511] after drain   : 0x%llx%s\n", (unsigned long long)out[5],
               out[5] ? "" : "   <- ZERO: Apple's deferred clearWithDMA wiped our entry");
        printf("  armed root / CONTEXT2   : 0x%llx / 0x%llx%s\n",
               (unsigned long long)out[6], (unsigned long long)out[7],
               out[6] == out[7] ? "   (they agree)" : "   *** THEY DISAGREE ***");
        // INSTRUMENT D. The positive control is printed FIRST and on its own, because a
        // zero there means the scan proved nothing about the ring - only about the query.
        // That is exactly the blindness that let the old PTEPDE-only scan reconcile
        // perfectly while measuring the wrong opcode (rule 72; "counting is not
        // witnessing").
        printf("  D positive control      : %llu PTEPDE(s) at root[0]%s\n",
               (unsigned long long)(out[8] & 0xffffffffull),
               (out[8] & 0xffffffffull) ? ""
                   : "   *** ZERO - THE SCAN IS UNPROVEN, read nothing into the counts below ***");
        printf("  D packets touching root : %llu\n", (unsigned long long)(out[8] >> 32));
        printf("  D packets walked        : %llu   (%llu IB(s) stopped on an unknown stride)\n",
               (unsigned long long)(out[9] >> 32), (unsigned long long)(out[9] & 0xffffffffull));
        printf("  D no dest / elsewhere   : %llu / %llu\n",
               (unsigned long long)(out[10] >> 32), (unsigned long long)(out[10] & 0xffffffffull));
        if (s >= 2)
            printf("  read back / fault       : 0x%llx / 0x%llx\n",
                   (unsigned long long)out[11], (unsigned long long)out[12]);
        // 0.0.264: the bounded poll, and the counters as TOTALS rather than as the
        // lower bound a capped log line gives. out[13] = (mapVA fires << 32) | unmapVA fires.
        printf("  mapVA / unmapVA fires   : %llu / %llu   (TOTALS from counters, not the "
               "capped log lines)\n",
               (unsigned long long)(out[13] >> 32), (unsigned long long)(out[13] & 0xffffffffull));
        if (out[14] & 0xffffffffull)
            printf("  CONTEXT2 bounded poll   : waited %llu ms -> %s\n",
                   (unsigned long long)(out[14] >> 32),
                   out[7] ? "CONTEXT2 went non-zero" : "TIMED OUT (staged doorbell is next, not a longer poll)");
        else
            printf("  CONTEXT2 bounded poll   : not needed - CONTEXT2 was already programmed\n");
        printf("  (every per-packet destination is in the driver log: `rootscan:` lines)\n");
    }
    if (in == 55) {
        // 0.0.250 extras, positionally. v[i] arrives at out[3 + i]:
        // 3 status, 4 guard that refused, 5 root page, 6 entry written, 7 entry read
        // back, 8 slot 511 before, 9 our L1 block offset, 10 ring VA, 11 (hdp << 1) |
        // tlb, 12 fault status after.
        // 0.0.252 REPACKED the last three (13 extras is a hard ceiling and all were
        // taken; the old out[15] wasted half its width repeating out[14]):
        // 13 (withdrawals << 32) | withdrawal refusals,
        // 14 (contexts patched << 32) | patch refusals,
        // 15 the four slot-37 counters as 16-bit CLAMPED fields:
        //    (fires << 48) | (root freed << 32) | (root survived << 16) | zero-at-entry.
        static const char *rwst[] = {
            "REFUSED - NOTHING was written into Apple's page table",
            "REPORT ONLY - the guards were evaluated, NOTHING was written",
            "*** WRITTEN and verified - our mapping is LIVE in Apple's table ***",
            "*** WRITTEN but a read-back MISMATCHED - treat this boot as void ***"
        };
        static const char *gname[] = {
            "none - every guard passed",
            "G1 identity (the context is not ours, not live, or already written)",
            "G2 arena range (the root is not a 4 KiB page inside Apple's arena)",
            "G3 root[0] is not a live root PDE pointing into the arena",
            "G4 slot 511 does not read exactly zero",
            ("G5 Apple has a PTEPDE queued at this root page, a decoded write covers slot 511, "
             "or (0.0.356) the root's block-clear is NOT witnessed as executed"),
            "G6 our own L1 block is not built and verified this boot",
            "(7 is not a write guard - G7 is the withdrawal's check)",
            "G8 (0.0.356) gfx power was measured at the write and read OFF"
        };
        const unsigned long long s = out[3];
        const unsigned long long g = out[4];
        // 0.0.356: modes 2/3 select WindowServer's context BY OWNER and pack extra
        // evidence into spare high bits: out[10] = ring VA | owner pid << 40; out[11] =
        // hdp/tlb | walk bits << 4 | walk-before bits << 6 | witness << 8 | identity re-read << 9 | owner << 10 |
        // CONTEXT2 agrees << 11 | power measured << 12 | power on << 13 | RLC_GPM_STAT
        // before << 32; out[12] = fault | RLC_GPM_STAT after << 32. Mask before printing.
        const unsigned long long rwMode = arg ? strtoull(arg, NULL, 0) & 0xffull : 0ull;
        const int byOwner = (rwMode == 2 || rwMode == 3);
        const unsigned long long faultLo = out[12] & 0xffffffffull;
        printf("  %-24s: %s\n", "rootwrite", rwst[s <= 3 ? s : 0]);
        // 0.0.251: 0xFF is the sentinel for "refused BEFORE the guards ran".
        // r88 printed "none - every guard passed" on that path because the scalar was simply
        // never set, which is a conclusion the command's own output contradicts (rule 23).
        if (g == 0xFF && byOwner)
            /* 0.0.357: the mode-1 sentence below described a check the by-owner modes never make. */
            printf("  guard verdict           : NOT EVALUATED - refused before the guards (no live context "
                   "created by a process that is WindowServer NOW has a page table, or the withdrawal path "
                   "was not live). The driver log's `rootwrite: by-owner field` lines list every record.\n");
        else if (g == 0xFF)
            printf("  guard verdict           : NOT EVALUATED - refused before the guards "
                   "(no live context's root agreed with CONTEXT2, or the withdrawal path "
                   "was not live). Run this WHILE A METAL CLIENT IS ALIVE.\n");
        else if (g <= 8)
            printf("  guard verdict           : %s\n", gname[g]);
        else
            printf("  guard verdict           : UNKNOWN guard number %llu - this CLI is older than "
                   "the kext; read the driver log's GUARD VERDICT line\n", g);
        printf("  Apple root page         : 0x%llx   (slot 511 before: 0x%llx)\n",
               (unsigned long long)out[5], (unsigned long long)out[8]);
        printf("  entry written / read    : 0x%llx / 0x%llx%s\n",
               (unsigned long long)out[6], (unsigned long long)out[7],
               (s >= 2 && out[6] == out[7]) ? "   <- match" :
               (s >= 2 ? "   *** MISMATCH ***" : ""));
        printf("  our L1 block            : vram+0x%llx   (ring VA 0x%llx)\n",
               (unsigned long long)out[9], (unsigned long long)(out[10] & 0xffffffffffull));
        if (byOwner && !(out[10] >> 40)) {
            /* 0.0.357: with no candidate the evidence bits below were never evaluated; printing them as
               FAILS / DISAGREES / FAILED claimed checks that did not run. */
            printf("  target (0.0.356)        : WindowServer BY OWNER - NO CANDIDATE, so no owner, CONTEXT2, identity,\n"
                   "                            witness, power or walk check was evaluated\n");
        } else if (byOwner) {
            const unsigned long long b = out[11];
            printf("  target (0.0.356)        : WindowServer BY OWNER, creator pid %llu%s\n",
                   (unsigned long long)(out[10] >> 40),
                   (out[10] >> 40) ? "" : "   (0 = no candidate was selected)");
            printf("  owner / CONTEXT2 / id   : owner evidence %s, CONTEXT2 %s, identity re-read %s\n",
                   (b >> 10) & 1 ? "HOLDS" : "FAILS", (b >> 11) & 1 ? "AGREES" : "DISAGREES",
                   (b >> 9) & 1 ? "passed" : "FAILED");
            printf("  block-clear witness     : %s\n", (b >> 8) & 1 ? "transition seen, root[0] read then"
                                                                     : "NONE for this record");
            if (s >= 2)
                printf("  gfx power at the write  : %s; RLC_GPM_STAT before 0x%08llx after 0x%08llx\n",
                       !((b >> 12) & 1) ? "NOT measured" : ((b >> 13) & 1) ? "ON (or RLC power gating off)"
                                                                        : "OFF/unknown",
                       (unsigned long long)(b >> 32), (unsigned long long)(out[12] >> 32));
            else   /* 0.0.357: nothing was written, so there is no "after" reading - it printed 0 */
                printf("  gfx power (report)      : %s; RLC_GPM_STAT 0x%08llx (no write, so no after-reading)\n",
                       !((b >> 12) & 1) ? "NOT measured" : ((b >> 13) & 1) ? "ON (or RLC power gating off)"
                                                                        : "OFF/unknown",
                       (unsigned long long)(b >> 32));
            printf("  walk BEFORE the arm     : ring VA %s, descriptor page %s   (the negative control only before the first write)\n",
                   (b >> 6) & 1 ? "RESOLVES" : "does NOT resolve", (b >> 7) & 1 ? "RESOLVES" : "does NOT resolve");
            printf("  walk through the root   : ring VA %s, descriptor page %s\n",
                   (b >> 4) & 1 ? "RESOLVES" : "does NOT resolve", (b >> 5) & 1 ? "RESOLVES" : "does NOT resolve");
        }
        // Only the write path flushes; on every other path saying "FAILED" claims a
        // failure that never happened.
        if (s >= 2)
            printf("  hdp flush / tlb flush   : %s / %s\n",
                   (out[11] & 2) ? "ok" : "FAILED", (out[11] & 1) ? "ok" : "FAILED");
        else
            printf("  hdp flush / tlb flush   : not attempted (nothing was written)\n");
        printf("  fault status after      : 0x%llx%s\n", faultLo,
               faultLo ? "   *** NON-ZERO - a VM fault was latched ***"
                       : "   (clean)");
        {
            /* 0.0.253: four CLAMPED 16-bit fields. The re-arm counters matter as much
               as the withdrawal ones: r89 measured the root surviving 10 of 11 unmaps,
               so a healthy armed boot shows withdrawals and re-arms rising together and
               differing by at most one (the teardown, where there is nothing to put
               back). Refusals on either side are the guards doing their job, but a
               non-zero count wants explaining before the boot is trusted. */
            const unsigned long long wd = (out[13] >> 48) & 0xffffu;
            const unsigned long long ra = (out[13] >> 32) & 0xffffu;
            const unsigned long long wr = (out[13] >> 16) & 0xffffu;
            const unsigned long long rr =  out[13]        & 0xffffu;
            printf("  withdrawals / re-arms   : %llu / %llu%s\n", wd, ra,
                   (wd >= ra && wd - ra <= 1) ? "   (balanced)"
                                              : "   *** UNBALANCED - check the driver log ***");
            printf("  withdraw / re-arm refus : %llu / %llu%s\n", wr, rr,
                   (wr || rr) ? "   <- a guard refused; see the driver log" : "   (none)");
        }
        printf("  contexts patched / ref  : %llu / %llu\n",
               (unsigned long long)(out[14] >> 32),
               (unsigned long long)(out[14] & 0xffffffffull));
        {
            /* 0.0.252, the slot-37 (unmapVA) observe counters. "root freed" is the
               teardown call the withdrawal must precede; "root survived" is how much
               re-arm traffic the final design would actually pay; "zero at entry" is
               the FALSIFIER - a non-zero count there means the hook point is still
               too late and the caller enumeration is incomplete. */
            const unsigned long long fires = (out[15] >> 48) & 0xffffu;
            const unsigned long long freed = (out[15] >> 32) & 0xffffu;
            const unsigned long long surv  = (out[15] >> 16) & 0xffffu;
            const unsigned long long zero  =  out[15]        & 0xffffu;
            printf("  unmapVA (slot 37) fires : %llu   root FREED by the call: %llu   "
                   "root survived: %llu\n", fires, freed, surv);
            printf("  root already 0 at entry : %llu%s\n", zero,
                   zero ? "   *** FALSIFIER - the hook point is too late ***" : "   (none)");
        }
        if (s < 2)
            printf("\nNOTHING was written into Apple's page table.\n");
        else
            printf("\nThe write LANDED. Verify with `accel vmib 0x23F0000000`: the walk must now\n"
                   "go root -> our L1 -> our leaf -> our physical page. The withdrawal runs\n"
                   "automatically on AMDHWVMContext::unmapVA (slot 37) - taken down at every unmap\n"
                   "and re-armed when the root survives it. (0.0.356: this line used to name\n"
                   "pageOffPD, slot 38, which is only the vtable anchor.)\n");
    }
    if (in == 51) {
        // 0.0.239 extras, positionally: 3 state, 4 cache entries, 5 resources scanned,
        // 6 windows, 7 candidates, 8 matches, 9 substituted, 10 read-back mismatches,
        // 11 refused|(ambiguous<<32), 12 miss-no-key, 13 miss-compare|(miss-short<<32),
        // 14 last VRAM address, 15 last resource offset|(bytes<<32). Rule 14: the result
        // must not depend on the shared driver log, which can hit capacity mid-run.
        const unsigned long long state = out[3];
        printf("  shadercache             : %s%s%s%s\n",
               (state & 1) ? "ARMED" : "not armed",
               (state & 2) ? ", blob open" : ", blob NOT open",
               (state & 4) ? ", residency hook live" : ", NO residency hook (needs skip-pagecopy + fire)",
               (state & 8) ? ", ADJUSTMENTS ACCEPTED (mode 3 — sound only for stages the draw policy "
                             "translates; nothing here applies a register)" : "");
        printf("  cache entries           : %llu\n", (unsigned long long)out[4]);
        printf("  resources scanned       : %llu  in %llu window(s)\n",
               (unsigned long long)out[5], (unsigned long long)out[6]);
        printf("  grid candidates         : %llu\n", (unsigned long long)out[7]);
        printf("  verified matches        : %llu\n", (unsigned long long)out[8]);
        printf("  SUBSTITUTED             : %llu%s\n", (unsigned long long)out[9],
               out[9] ? "   *** gfx1201 code written over Apple's by keyed lookup ***" : "");
        printf("  read-back MISMATCHED    : %llu\n", (unsigned long long)out[10]);
        printf("  refused / ambiguous     : %llu / %llu\n",
               (unsigned long long)(out[11] & 0xffffffffu), (unsigned long long)(out[11] >> 32));
        printf("  misses: no key          : %llu\n", (unsigned long long)out[12]);
        printf("  misses: bytes differ    : %llu   too few bytes: %llu\n",
               (unsigned long long)(out[13] & 0xffffffffu), (unsigned long long)(out[13] >> 32));
        printf("  last substitution       : VRAM 0x%llx  resource +0x%llx  %llu byte(s)\n",
               (unsigned long long)out[14], (unsigned long long)(out[15] & 0xffffffffu),
               (unsigned long long)(out[15] >> 32));
    }
    if (in == 49) {
        // 0.0.235 extras, positionally: 3 armed, 4 entries, 5 calls, 6 skipped,
        // 7 retired, 8 refused, 9 lastReason, 10 lastChan, 11 lastSubmitted,
        // 12 lastCompleted, 13 lastAfter. Rule 14: the result must not depend on
        // the shared driver log, which can hit capacity mid-run.
        static const char *why[] = { "none", "no Hardware pointer",
                                     "ring hooks not installed (no slide)",
                                     "slide source page offset wrong",
                                     "slide not page aligned",
                                     "computed checkTimestamps fails geometry",
                                     "no channel object", "iface/vtable/scheduler unusable",
                                     "per-boot call cap spent" };
        const unsigned long long reason = (unsigned long long)out[9];
        printf("  eopbridge               : %s\n", out[3] ? "ARMED" : "not armed");
        printf("  EOP entries seen        : %llu\n", (unsigned long long)out[4]);
        printf("  checkTimestamps calls   : %llu\n", (unsigned long long)out[5]);
        printf("  nothing outstanding     : %llu\n", (unsigned long long)out[6]);
        printf("  channels RETIRED        : %llu%s\n", (unsigned long long)out[7],
               out[7] ? "   *** the interrupt drove Apple's own retire ***" : "");
        printf("  refusals                : %llu  last reason %llu (%s)\n",
               (unsigned long long)out[8], reason, reason <= 8 ? why[reason] : "?");
        printf("  last chan / sub / comp  : %llu / %llu / %llu -> %llu\n",
               (unsigned long long)out[10], (unsigned long long)out[11],
               (unsigned long long)out[12], (unsigned long long)out[13]);
        // 0.0.270 : 14 SDMA trap entries (IH client 0x0a src 49), 15 checkTimestamps calls they made
        printf("  SDMA trap entries / calls: %llu / %llu\n", (unsigned long long)out[14], (unsigned long long)out[15]);
    }
    if (in == 34 || in == 35) {
        // 0.0.176 extras, positionally: 3 ok, 4 chan, 5 id, 6 submitted,
        // 7 completed, 8 pollState (bit 8 = PATH B), 9 baseline, 10 fires,
        // 11 fallbacks, 12 lastWritten, 13 lastPropMs, 14 wbPtr, 15 wbValue.
        static const char *st[] = { "never armed", "ARMED (still polling)",
                                    "FIRED", "TIMED OUT (nothing past baseline)",
                                    "SETTLED" };
        const uint64_t ok    = out[3];
        const uint64_t state = out[8] & 0xFF;
        const int pathB      = (out[8] & 0x100) != 0;
        printf("  %-24s: %s\n", in == 34 ? "kiqstamp" : "kiqchan",
               ok ? (in == 34 ? "ARMED" : "read") : "REFUSED");
        printf("  KIQ channel             : 0x%llx  id = %llu%s\n",
               (unsigned long long)out[4], (unsigned long long)out[5],
               out[5] == 1 ? "" : "   *** expected 1 — nothing was written ***");
        printf("  submitted (chan+0x80)   : 0x%llx\n", (unsigned long long)out[6]);
        printf("  completed (chan+0x84)   : 0x%llx\n", (unsigned long long)out[7]);
        printf("  baseline at arm         : 0x%llx\n", (unsigned long long)out[9]);
        printf("  writeback [chan+0xc0]   : 0x%llx  *ptr = 0x%llx\n",
               (unsigned long long)out[14], (unsigned long long)out[15]);
        printf("  timestampUpdated path   : %s\n",
               pathB ? "B — 0xbe08548 copies *[0xc0] into +0x84 (chan+0xe0 == -1)"
                     : "A — AMDSWScheduler::timestampUpdated (chan+0xe0 != -1)");
        printf("  poller state            : %s\n",
               state < 5 ? st[state] : "?");
        printf("  fires / fallbacks       : %llu / %llu%s\n",
               (unsigned long long)out[10], (unsigned long long)out[11],
               out[11] ? "   *** a FALLBACK means Apple never propagated ***" : "");
        if (out[10])
            printf("  last stamp written      : 0x%llx\n", (unsigned long long)out[12]);
        if (out[13] == 0xFFFFFFFFull)
            printf("  propagation             : NONE observed yet\n");
        else
            printf("  propagation             : ~%llu ms (Apple did the +0x84 store)\n",
                   (unsigned long long)out[13]);
        if (in == 34 && ok)
            printf("\nNow run: navi48test accel pm4powerup, then navi48test accel kiqchan\n");
        if (in == 35 && ok)
            printf("\nThe KIQ ring dump and the PM4 decode are in `navi48test log`.\n");
    }
    if (in == 36 || in == 37) {
        // 0.0.178 extras, positionally: 3 ok, 4 armed|proceed<<1|shadows<<8,
        // 5 frames, 6 stamps, 7 refusals, 8 mapResult, 9 failStep,
        // 10 addKr<<32|removeKr, 11 mqdGpu, 12 doorbell,
        // 13 wptrHi<<32|wptr, 14 active<<32|rptr, 15 appleRingWptr<<32|cntl.
        static const char *mr[] = { "not attempted", "MAPPED", "REFUSED" };
        static const char *fs[] = { "-", "not ready", "a page is NOT RESIDENT",
                                    "the wptr write-back is bogus",
                                    "MES REMOVE_QUEUE", "the MQD build",
                                    "MES ADD_QUEUE" };
        const uint64_t ok      = out[3];
        const int armed        = (out[4] & 1) != 0;
        const int proceed      = (out[4] & 2) != 0;
        const uint64_t shadows = out[4] >> 8;
        const uint64_t result  = out[8];
        const uint64_t step    = out[9];
        printf("  %-24s: %s\n", in == 36 ? "gfxmap" : "gfxstate",
               ok ? (in == 36 ? "ARMED" : "read") : "REFUSED");
        printf("  emulator                : %s ; proceed = %s%s\n",
               armed ? "armed" : "not armed", proceed ? "TRUE" : "false",
               proceed ? "   <-- the gate shadow is OFF; doStart sees the truth"
                       : "   (a live-zero CP_RB0_RPTR is answered with 1)");
        printf("  gate shadows applied    : %llu\n", (unsigned long long)shadows);
        printf("  frames / stamps / refus : %llu / %llu / %llu%s\n",
               (unsigned long long)out[5], (unsigned long long)out[6],
               (unsigned long long)out[7],
               out[7] ? "   *** a WRITE_DATA went somewhere unexpected ***" : "");
        printf("  map result              : %s\n", result < 3 ? mr[result] : "?");
        if (result == 2)
            printf("  refused at step         : %llu (%s)\n",
                   (unsigned long long)step, step < 7 ? fs[step] : "?");
        printf("  MES REMOVE / ADD        : 0x%x / 0x%x\n",
               (unsigned)(out[10] & 0xFFFFFFFFu), (unsigned)(out[10] >> 32));
        printf("  MQD handed to MES       : 0x%llx  doorbell dword index %llu\n",
               (unsigned long long)out[11], (unsigned long long)out[12]);
        if (in == 37) {
            printf("  CP_RB0_RPTR    (0x21c0) : 0x%08x%s\n",
                   (unsigned)(out[14] & 0xFFFFFFFFu),
                   (out[14] & 0xFFFFFFFFu) ? "" : "   <- idle, as doStart's gate wants");
            printf("  CP_RB0_WPTR    (0x3054) : 0x%08x  _HI (0x3055) 0x%08x\n",
                   (unsigned)(out[13] & 0xFFFFFFFFu), (unsigned)(out[13] >> 32));
            printf("  CP_RB0_CNTL    (0x3041) : 0x%08x%s\n",
                   (unsigned)(out[15] & 0xFFFFFFFFu),
                   ((out[15] & 0xFFFFFFFFu) == 0x00f00e10u)
                       ? "   <- bufsz 16: APPLE'S 0x80000-byte ring" : "");
            printf("  CP_GFX_HQD_ACT (0x30e0) : 0x%08x\n", (unsigned)(out[14] >> 32));
            printf("  Apple GFX ring wptr     : %llu dwords%s\n",
                   (unsigned long long)(out[15] >> 32),
                   (out[15] >> 32) ? "   *** performClearState SUBMITTED ***" : "");
        }
        if (in == 36 && ok)
            printf("\nNow run: navi48test accel pm4powerup, then navi48test accel gfxstate\n");
        if (in == 37)
            printf("\nThe ring dump, the frame decode and the write-back values are in "
                   "`navi48test log`.\n");
    }
    if (in == 38 || in == 39) {
        // 0.0.185 extras, positionally: 3 ok, 4 armed|q1Test<<8, 5 mapResult,
        // 6 failStep, 7 doorbell, 8 ringGpu, 9 wptr<<32|dwords, 10 rptrReport,
        // 11 wptrWb, 12 regWptr<<32|regRptr, 13 regDoorbell<<32|regCntl,
        // 14 regDoorbellOff<<32|fence, 15 the doorbell POINTER at ring->0xc0.
        static const char *mr[] = { "not attempted", "MAPPED", "REFUSED" };
        static const char *fs[] = { "-", "no chan id 14 (has `fire` run?)",
                                    "the ring failed its vtable identity check",
                                    "ring VA / size / wptr / flags not programmable",
                                    "a page is NOT RESIDENT (run `synctables`)",
                                    "SDMA0 QUEUE1 refused, or read back wrong",
                                    "could not resolve the BAR2 doorbell dword",
                                    "ring->0xc0 is not the word our TTL handed out",
                                    "the boot self-test says 0x202 does not route" };
        static const char *q1[] = { "never ran (navi48-sdma-q1-test=1 arms it)",
                                    "the doorbell ROUTES to SDMA0 QUEUE1",
                                    "MMIO only - the doorbell does NOT route",
                                    "QUEUE1 did not execute at all" };
        const uint64_t ok     = out[3];
        const int armed       = (out[4] & 1) != 0;
        const uint64_t q1res  = (out[4] >> 8) & 0xFF;
        const uint64_t result = out[5];
        const uint64_t step   = out[6];
        printf("  %-24s: %s\n", in == 38 ? "sdmamap" : "sdmastate",
               ok ? (in == 38 ? "ARMED" : "read") : "REFUSED");
        printf("  takeover                : %s\n", armed ? "ARMED" : "not armed");
        printf("  Apple rings mapped      : %llu   unmapped: %llu\n",
               (unsigned long long)((out[4] >> 16) & 0xFF),
               (unsigned long long)((out[4] >> 24) & 0xFF));
        printf("  QUEUE1 routing selftest : %s\n", q1res < 4 ? q1[q1res] : "?");
        printf("  map result              : %s\n", result < 3 ? mr[result] : "?");
        if (result == 2)
            printf("  refused at step         : %llu (%s)\n",
                   (unsigned long long)step, step < 9 ? fs[step] : "?");
        printf("  doorbell dword index    : 0x%llx  ring->0xc0 = 0x%llx\n",
               (unsigned long long)out[7], (unsigned long long)out[15]);
        printf("  Apple SDMA ring         : 0x%llx  %llu dwords, wptr %llu\n",
               (unsigned long long)out[8],
               (unsigned long long)(out[9] & 0xFFFFFFFFull),
               (unsigned long long)(out[9] >> 32));
        printf("  rptr report (+0xb8)     : 0x%llx%s\n",
               (unsigned long long)out[10],
               out[10] ? "" : "   <- zero: RB_RPTR_ADDR uses OUR writeback slot");
        printf("  wptr write-back (+0xd0) : 0x%llx   frame base 0x%llx\n",
               (unsigned long long)out[11],
               (unsigned long long)(out[11] ? out[11] - 0x10 : 0));
        printf("  QUEUE1 RB_RPTR / WPTR   : 0x%08x / 0x%08x%s\n",
               (unsigned)(out[12] & 0xFFFFFFFFu), (unsigned)(out[12] >> 32),
               (out[12] >> 32) ? "   *** the doorbell ROUTED - Apple's wptr reached the engine ***" : "");
        printf("  QUEUE1 RB_CNTL          : 0x%08x  (RB_ENABLE=%u RB_SIZE=%u)\n",
               (unsigned)(out[13] & 0xFFFFFFFFu),
               (unsigned)(out[13] & 1u), (unsigned)((out[13] >> 1) & 0x1Fu));
        printf("  QUEUE1 DOORBELL / OFF   : 0x%08x / 0x%08x  (index 0x%x)\n",
               (unsigned)(out[13] >> 32), (unsigned)(out[14] >> 32),
               (unsigned)((out[14] >> 32) & 0x0FFFFFFCu) >> 2);
        printf("  FENCE target dword      : 0x%08x%s\n",
               (unsigned)(out[14] & 0xFFFFFFFFu),
               (out[14] & 0xFFFFFFFFu) ? "   *** the FENCE FIRED - the engine ran Apple's frame ***" : "   (not fired)");
        if (in == 38 && ok)
            printf("\nNow: read `navi48test log`, then run blit2, then "
                   "navi48test accel sdmastate WHILE IT IS STILL ALIVE (rule 26).\n");
        if (in == 39)
            printf("\nThe ring dump, the packet decode and the QUEUE1 registers are in "
                   "`navi48test log`.\n");
    }
    if (in == 40 || in == 41) {
        const uint64_t ok = out[3];
        printf("  %-24s: %s\n", in == 40 ? "faultclear" : "vmstate",
               ok ? (in == 40 ? (ok == 1 ? "CLEARED (status reads 0)"
                                         : "cleared, but a fault re-latched immediately")
                              : "read")
                  : "REFUSED");
        if (in == 41 && ok)
            printf("  gfx12-encoded entries   : %llu of 4 probed\n",
                   (unsigned long long)(ok - 1));
        printf("\nThe decoded fault status, GCVM_CONTEXT2 and the arena entries are "
               "in `navi48test log`.\n");
    }
    if (in == 42) {
        static const char *why[] = { "REFUSED", "root PDE not valid",
                                     "L1 entry not valid", "leaf entry not valid",
                                     "the page could not be read",
                                     "dumped from VRAM", "dumped from a HOST page" };
        const uint64_t ok = out[3];
        printf("  %-24s: %s\n", "vmib", why[ok <= 6 ? ok : 0]);
        printf("  page (physical)         : 0x%llx\n", (unsigned long long)out[4]);
        printf("  first dword             : 0x%08x\n", (unsigned)out[5]);
        printf("  CP_STAT                 : 0x%08x\n", (unsigned)out[6]);
        printf("\nThe page-table walk, the 0x80-dword dump and the PM4 decode are "
               "in `navi48test log`.\n");
    }
    if (in == 43) {
        const uint64_t ok = out[3];
        printf("  %-24s: %s\n", "ringib", ok ? "ring decoded" : "REFUSED");
        printf("  channel                 : %llu%s\n",
               (unsigned long long)out[4],
               out[4] ? "" : "   (0 = chan 14, the ring section 304 proved executes)");
        printf("  indirect buffers decoded: %llu\n",
               (unsigned long long)(ok ? ok - 1 : 0));
        printf("\nThe ring dump, the packet decode and every IB's decode are in "
               "`navi48test log`.\n");
    }
    if (in == 44) {
        const uint64_t st = out[3];
        printf("  %-24s: %s, %s, %s\n", "pagecopy", (st & 1) ? "ARMED" : "not armed",
               (st & 2) ? "skip-pagecopy hook active" : "skip-pagecopy hook OFF",
               (st & 4) ? "a resource vtable is patched" : "no resource patched yet");
        printf("  copies (sysmem->VRAM)   : %llu\n", (unsigned long long)out[4]);
        printf("  bytes copied            : 0x%llx\n", (unsigned long long)out[5]);
        printf("  read-back dwords        : %llu\n", (unsigned long long)out[6]);
        printf("  read-back MISMATCHED    : %llu\n", (unsigned long long)out[7]);
        printf("  unhandled (skipped)     : %llu (reason mask 0x%llx)\n",
               (unsigned long long)out[8], (unsigned long long)out[9]);
        printf("  page-outs (skipped)     : %llu\n", (unsigned long long)out[10]);
        printf("  last destination (VRAM) : 0x%llx + 0x%llx bytes\n",
               (unsigned long long)out[11], (unsigned long long)out[12]);
        printf("  last copy time          : %llu us\n", (unsigned long long)out[13]);
        printf("  failed mid-copy         : %llu\n", (unsigned long long)out[14]);
        printf("  pageTexture skips       : %llu\n", (unsigned long long)out[15]);
        printf("\nThe per-copy lines (residency-copy: ...) are in `navi48test log`.\n");
    }
    if (in == 45) {
        static const char *why[] = { "REFUSED - nothing written",
                                     "REWRITTEN (partial flush after the dispatch -> two NOPs, read back)",
                                     "already rewritten", "written but read-back MISMATCHED" };
        const uint64_t st = out[3];
        printf("  %-24s: %s\n", "flushdrop", why[st <= 3 ? st : 0]);
        printf("  blit IB (VMID 2) VA     : 0x%llx, %llu dwords\n",
               (unsigned long long)out[4], (unsigned long long)out[5]);
        printf("  host page (physical)    : 0x%llx\n", (unsigned long long)out[6]);
        printf("  DISPATCH_DIRECT at dword: 0x%llx\n", (unsigned long long)out[7]);
        printf("  rewritten dwords at     : 0x%llx\n", (unsigned long long)out[8]);
        printf("  PTEPDE entries collected: %llu (from %llu SDMA IBs)\n",
               (unsigned long long)out[9], (unsigned long long)out[10]);
        printf("  read-back mismatches    : %llu\n", (unsigned long long)out[11]);
        printf("  pending pages scanned   : %llu, matches: %llu (0.0.203 content search; 0 when the ring named the IB)\n",
               (unsigned long long)out[12], (unsigned long long)out[13]);
        printf("\nThe ring walk, the page-table lookups and the IB identity check are in "
               "`navi48test log`.\n");
    }
    if (in == 46) {
        static const char *why[] = { "REFUSED - nothing written",
                                     "SUBSTITUTED (gfx1201 kernel written, read back)",
                                     "already substituted", "written but read-back MISMATCHED" };
        const uint64_t st = out[3];
        printf("  %-24s: %s\n", "kernsub", why[st <= 3 ? st : 0]);
        printf("  shader VRAM address     : 0x%llx\n", (unsigned long long)out[4]);
        printf("  gfx1201 kernel bytes    : %llu\n", (unsigned long long)out[5]);
        printf("  read-back MISMATCHED    : %llu\n", (unsigned long long)out[6]);
        printf("  substitutions this boot : %llu\n", (unsigned long long)out[7]);
        printf("  last refusal reason     : %llu (1 guard, 2 read, 3 not Apple's kernel, 4 write, 5 another mode armed)\n",
               (unsigned long long)out[8]);
        printf("  armed                   : %llu\n", (unsigned long long)out[9]);
        printf("  residency copy lastDst  : 0x%llx\n", (unsigned long long)out[10]);
        static const char *modes[] = { "none", "1 blit_copy_gfx1201 (copy)",
                                       "2 blit_diag_gfx1201 (INSTRUMENT)",
                                       "3 blit_diagmin_gfx1201 (INSTRUMENT)",
                                       "4 blit_copy_offen_gfx1201 (copy by byte offset)" };
        printf("  kernel mode             : %s\n", modes[out[11] <= 4 ? out[11] : 0]);
        printf("  kernel dwords           : %llu\n", (unsigned long long)out[12]);
        printf("\nThe kernsub/residency-copy substitution lines are in `navi48test log`.\n");
    }
    if (in == 47) {
        static const char *why[] = { "REFUSED", "root PDE not valid",
                                     "L1 entry not valid", "leaf entry not valid",
                                     "the page could not be read",
                                     "scanned a VRAM page", "scanned a HOST page" };
        const uint64_t ok = out[3];
        printf("  %-24s: %s\n", "vmpage", why[ok <= 6 ? ok : 0]);
        printf("  vmpage page (physical)  : 0x%llx\n", (unsigned long long)out[4]);
        printf("  vmpage decoder self-test: %s\n", out[5] ? "ok (planted record found)" : "FAILED - the counts below mean nothing");
        printf("  vmpage nonzero dwords   : %llu of 1024\n", (unsigned long long)out[6]);
        printf("  vmpage sentinel A hits  : %llu (0x600d600d)\n", (unsigned long long)out[7]);
        printf("  vmpage sentinel B hits  : %llu (0x600d0b0b)\n", (unsigned long long)out[8]);
        printf("  vmpage hits at d3 slots : %llu (dword index 3 mod 4)\n", (unsigned long long)out[9]);
        if (out[10] == 0xffffffffull)
            printf("  vmpage first hit        : none\n");
        else
            printf("  vmpage first hit        : dword 0x%llx (byte 0x%llx)\n",
                   (unsigned long long)out[10], (unsigned long long)out[10] * 4);
        printf("  vmpage hit pitch        : %llu dwords (gcd of gaps; 4 = 16-byte records, 16 = 64-byte)\n",
               (unsigned long long)out[11]);
        printf("  vmpage A d0 min/max     : 0x%llx / 0x%llx\n",
               (unsigned long long)out[12], (unsigned long long)out[13]);
        printf("  vmpage A d0 values seen : 0x%016llx (bit n = d0 n; bit 63 = d0 >= 63)\n",
               (unsigned long long)out[14]);
        printf("  vmpage d2 (v0) max      : 0x%llx\n", (unsigned long long)out[15]);
        printf("\nThe page walk, the nonzero lines and one line per decoded record are in "
               "`navi48test log` (vmpage: ...).\n");
    }
    if (in == 48) {
        // 0.0.209 layout : out[3+k] = extra[k].
        static const char *st[] = { "REFUSED - nothing written", "WRITTEN and read back", "already written this boot",
                                    "written but read-back MISMATCHED", "?", "census only (read-only)" };
        const uint64_t *x = out + 3;
        static const char *md[] = { "0 census", "1 translate in place", "2 (retired)", "3 BLANK (instrument)",
                                    "4 DRAW translate in place (m2tri)", "5 DRAW translate read-only (m2tri)" };
        printf("  renderxlat status        : %llu (%s)\n", (unsigned long long)x[0], st[x[0] <= 5 ? x[0] : 4]);
        // 0.0.224 : modes 6/7, the suite - out[11] refusing status | segments << 16 | refused segment << 24 | err op
        // << 32; out[12] identified | pages written << 16 | smallest pad << 32
        if (((x[1] & 0xF) == 6 || (x[1] & 0xF) == 7) && ((x[1] >> 4) & 0xF))
            printf("  suite (modes 6/7)        : passes %llu; segments found %llu, identified %llu; refused at segment %llu status %llu "
                   "err op 0x%02llx (0xff = none); pages written %llu; smallest NOP pad %llu dw\n",
                   (unsigned long long)((x[1] >> 4) & 0xF), (unsigned long long)((x[11] >> 16) & 0xFF),
                   (unsigned long long)(x[12] & 0xFFFF), (unsigned long long)((x[11] >> 24) & 0xFF),
                   (unsigned long long)(x[11] & 0xFFFF), (unsigned long long)((x[11] >> 32) & 0xFF),
                   (unsigned long long)((x[12] >> 16) & 0xFFFF), (unsigned long long)(x[12] >> 32));
        printf("  mode / boot write mode  : %s / %llu ; self-test %s ; NGG seeded %s\n",
               ((x[1] & 0xF) == 6 && ((x[1] >> 4) & 0xF) ? "6 SUITE translate in place (0.0.224)" : (x[1] & 0xF) == 7 && ((x[1] >> 4) & 0xF) ? "7 SUITE translate read-only (0.0.224)" : md[(x[1] & 0xFF) <= 5 ? (x[1] & 0xFF) : 2]), (unsigned long long)((x[1] >> 16) & 0xFF),
               ((x[1] >> 8) & 1) ? "ok" : "FAILED/not run", ((x[1] >> 9) & 1) ? "yes" : "no");
        printf("  PM4 streams / unreadable runs : %llu / %llu  (pages read %llu, pending PTEPDE entries %llu)\n",
               (unsigned long long)(x[5] & 0xFF), (unsigned long long)((x[5] >> 8) & 0xFF),
               (unsigned long long)((x[5] >> 16) & 0xFFFF), (unsigned long long)(x[5] >> 32));
        printf("  primary stream VA / page: 0x%llx / 0x%llx\n", (unsigned long long)x[2], (unsigned long long)x[3]);
        printf("  walk / end-NOP-128 / early / ctxctl-first : %llu / %llu / %llu / %llu\n",
               (unsigned long long)(x[4] & 0xFFFF), (unsigned long long)((x[4] >> 16) & 0xF),
               (unsigned long long)((x[4] >> 20) & 0xF), (unsigned long long)((x[4] >> 24) & 1));
        printf("  packets; SET ctx/sh-gfx/sh-cs/ucfg/index : %llu; %llu/%llu/%llu/%llu/%llu\n",
               (unsigned long long)(x[6] & 0xFFFF), (unsigned long long)((x[6] >> 16) & 0xFF),
               (unsigned long long)((x[6] >> 24) & 0xFF), (unsigned long long)((x[6] >> 32) & 0xFF),
               (unsigned long long)((x[6] >> 40) & 0xFF), (unsigned long long)((x[6] >> 48) & 0xFF));
        printf("  regs id/mv/rp/absent/legacyVS/UNKNOWN/cs-proven : %llu/%llu/%llu/%llu/%llu/%llu/%llu\n",
               (unsigned long long)(x[7] & 0xFFFF), (unsigned long long)((x[7] >> 16) & 0xFFFF),
               (unsigned long long)((x[7] >> 32) & 0xFFFF), (unsigned long long)((x[7] >> 48) & 0xFFFF),
               (unsigned long long)(x[8] & 0xFFFF), (unsigned long long)((x[8] >> 16) & 0xFFFF),
               (unsigned long long)(x[8] >> 32));
        if (((x[1] & 0xFF) == 4) || ((x[1] & 0xFF) == 5))
            printf("  draw (modes 4/5)        : translate status %llu out %llu dw err op 0x%llx; shaders VS %s PS %s; "
                   "NOP pad %llu dw; err reg 0x%llx; read-back mismatches %llu\n",
                   (unsigned long long)(x[11] & 0xFFFF), (unsigned long long)((x[11] >> 16) & 0xFFFF),
                   (unsigned long long)((x[11] >> 32) & 0xFF), ((x[12] >> 32) & 1) ? "OURS" : "not ours",
                   ((x[12] >> 33) & 1) ? "OURS" : "not ours", (unsigned long long)((x[12] >> 40) & 0xFFFF),
                   (unsigned long long)(x[12] & 0xFFFFFFFF), (unsigned long long)((x[11] >> 40) & 0xFFFF));
        if (((x[1] & 0xFF) == 4) || ((x[1] & 0xFF) == 5))
            printf("  GE rings / CU (0.0.217)  : requested %s, ring pages found %llu of 2688, ring check %s, RSRC3_GS %s, "
                   "extra block %llu dw; preamble delta (0.0.218) %s, retired bit 0x800 (0.0.219) %s, raster/CB delta (0.0.220) %s\n",
                   ((x[1] >> 42) & 1) ? "yes" : "no", (unsigned long long)((x[1] >> 24) & 0xFFFF),
                   ((x[1] >> 40) & 1) ? "ok" : "--", ((x[1] >> 41) & 1) ? "requested" : "not requested",
                   (unsigned long long)((x[12] >> 56) & 0xFF), ((x[1] >> 43) & 1) ? "requested" : "no",
                   ((x[1] >> 44) & 1) ? "GIVEN - REFUSED" : "no", ((x[1] >> 45) & 1) ? "requested" : "no");
        printf("  memory-loaded: CLEAR_STATE %llu LOAD_* %llu CTXCTL other %llu (proven %llu)\n",
               (unsigned long long)(x[9] & 0xFF), (unsigned long long)((x[9] >> 8) & 0xFF),
               (unsigned long long)((x[9] >> 16) & 0xFF), (unsigned long long)((x[9] >> 24) & 0xFF));
        printf("  COND_EXEC %llu nested-IB %llu DMA_DATA %llu bad-reg-operand %llu ; draws %llu dispatches %llu unlisted %llu (first op 0x%02llx)\n",
               (unsigned long long)((x[9] >> 32) & 0xFF), (unsigned long long)((x[9] >> 40) & 0xFF),
               (unsigned long long)((x[9] >> 48) & 0xFF), (unsigned long long)((x[9] >> 56) & 0xFF),
               (unsigned long long)(x[10] & 0xFFFF), (unsigned long long)((x[10] >> 16) & 0xFFFF),
               (unsigned long long)((x[10] >> 32) & 0xFFFF), (unsigned long long)((x[10] >> 48) & 0xFF));
        printf("  first UNKNOWN reg / first memory-loaded op : 0x%llx / 0x%02llx\n",
               (unsigned long long)(x[12] & 0xFFFFFFFFull), (unsigned long long)((x[12] >> 32) & 0xFF));
        printf("  primary stream in page  : %s page, dword offset 0x%llx\n", ((x[12] >> 56) & 1) ? "HOST" : "VRAM",
               (unsigned long long)((x[12] >> 40) & 0x3FF));
        printf("  write status / out dw / err op / read-back mismatches : %llu / %llu / 0x%02llx / %llu\n",
               (unsigned long long)(x[11] & 0xFFFF), (unsigned long long)((x[11] >> 16) & 0xFFFF),
               (unsigned long long)((x[11] >> 32) & 0xFF), (unsigned long long)(x[11] >> 40));
        printf("\nThe candidates, their census, the page dumps (renderib: candN pg[...]) and the write are in "
               "`navi48test log`; decode offline with tools/pm4-xlat-decode.py.\n");
    }
    if (in == 1) printf("\nNow run: navi48test log    (the TTL call trace is the result)\n");
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s info|counters|reg <dw>|regs (list on stdin)|poke|submit|log|metrics|power <n>|logreset|\n       accel [status|fire [1 gate closed|2 gate open]|memenable|synctables|enablerings|startengines|ringstate|dumpring|programqueue|ringhooks|dumpib|rebaseib|enablequeue|kickdoorbell|ringrefs|queuestate|opengate|neuterpoll|chanstate|pokecompletion|signalcompletion|bindchannel|schedstate|stampstate|signalstamp|stampgap|runcheckts|runadvance|xlatregs|srbmprobe|resume|pm4powerup|setvspace|kiqenable|kiqstamp|kiqchan|gfxmap|gfxstate|sdmamap|sdmastate|faultclear|vmstate|vmib [va]|ringib [chan]|pagecopy [1]|flushdrop|kernsub [1|2|3]|vmpage [va]|renderxlat [0|1|3|4|5|0x101|base|flags|n<<4|6 or 7]|eopbridge [1|2]|bootchain [mode]|shadercache [1|2|3]|vmroots [addr]|ringmap [0|1]|vmctx|rootwrite [0|1]|rearmdrain [0|1]|pairing [1|2]|drain|flushhook [1|2|3]|scanout [0|1|2|3|4|5|6|7 <vramOff> [gcr]|8|full [file]|10]|sdmadcc [0|1|2]|gfxcensus [1|2]|gfxneuter [1|2]|finishread|gfxcapture [1|2]|gfxprobe [1|2]|pipeguard [1]|agdc [1]|agdchold [ms 1..5000]|cqprobe|ucprobe [0|1|2]|fbbench [rows]|fbwc [1]|pipeshim [0|1|2|3|4|5]|pipemode [0|1]|emcensus [0|1|2]|routea [0|1|2]|dcnstate [1]|dcnvbl [0|1|2]|dcnflip [0|1|2..30|1002..1240]|dcnmode [0|1..30|101..130]|fbname [0|1]|pipeadopt|pipearm [0|1]|pipestat [0|1|2|3|4]|pipestamps|pipeshortcut [0|1]|pipeagdc [0|1]|pipevbl [0|1]|pipereload [0|1]]|capstream <file> [ms] [s] [stopfile]|bigmem [mb]|test <id> <iters> [us]|suite [iters]\n", argv[0]);
        return 2;
    }
    if (open_service() < 0) return 1;
    int rc = 2;
    if      (!strcmp(argv[1], "info"))     rc = cmd_info();
    else if (!strcmp(argv[1], "counters")) rc = cmd_counters();
    else if (!strcmp(argv[1], "reg") && argc > 2) rc = cmd_reg(argv[2]);
    else if (!strcmp(argv[1], "regs"))     rc = cmd_regs();
    else if (!strcmp(argv[1], "poke"))     rc = cmd_poke();
    else if (!strcmp(argv[1], "submit"))   rc = cmd_submit();
    else if (!strcmp(argv[1], "log"))      rc = cmd_log();
    else if (!strcmp(argv[1], "logstream") && argc > 2)
        rc = cmd_logstream(argv[2], argc > 3 ? atoi(argv[3]) : 1000, argc > 4 ? atoi(argv[4]) : 3600, argc > 5 ? argv[5] : NULL);
    else if (!strcmp(argv[1], "capstream") && argc > 2)
        rc = cmd_capstream(argv[2], argc > 3 ? atoi(argv[3]) : 250, argc > 4 ? atoi(argv[4]) : 3600, argc > 5 ? argv[5] : NULL);
    else if (!strcmp(argv[1], "logreset")) {
        kern_return_t kr = IOConnectCallMethod(conn, kNavi48SelLogReset, NULL, 0, NULL, 0,
                                               NULL, NULL, NULL, NULL);
        if (kr != KERN_SUCCESS) { fprintf(stderr, "LogReset failed 0x%x\n", kr); rc = 1; }
        else { printf("driver log cleared\n"); rc = 0; }
    }
    else if (!strcmp(argv[1], "metrics"))  rc = cmd_metrics(argc > 2 ? atoi(argv[2]) : 1);
    else if (!strcmp(argv[1], "power") && argc > 2) {
        uint64_t st = (uint64_t)atoi(argv[2]);
        kern_return_t kr = IOConnectCallScalarMethod(conn, kNavi48SelPowerState, &st, 1, NULL, NULL);
        static const char *n[] = {"auto","low","nominal","high","peak"};
        printf("power state -> %s: %s (0x%x)\n", st < 5 ? n[st] : "?",
               kr == KERN_SUCCESS ? "ok" : "FAILED", kr);
        rc = (kr == KERN_SUCCESS) ? 0 : 1;
    }
    else if (!strcmp(argv[1], "accel"))    rc = cmd_accel(argc > 2 ? argv[2] : "status", argc > 3 ? argv[3] : NULL,
                                                          argc > 4 ? argv[4] : NULL, argc > 5 ? argv[5] : NULL);
    else if (!strcmp(argv[1], "bigmem"))   rc = cmd_bigmem(argc > 2 ? (uint64_t)atoll(argv[2]) : 1024);
    else if (!strcmp(argv[1], "test") && argc > 3)
        rc = run_test((uint32_t)atoi(argv[2]), (uint32_t)atoi(argv[3]),
                      argc > 4 ? (uint32_t)atoi(argv[4]) : 1000000, 0);
    else if (!strcmp(argv[1], "suite"))
        rc = cmd_suite(argc > 2 ? (uint32_t)atoi(argv[2]) : 10000);
    else fprintf(stderr, "unknown command %s\n", argv[1]);
    IOServiceClose(conn);
    return rc;
}
