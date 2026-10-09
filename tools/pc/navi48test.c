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
#include "../../src/navi48-bringup/src/dcn/navi48_dispread.h" /* 0.0.622: ddcread / dmubring / dispcensus - statuses, argument packing, the census table, one source of truth with the kext */
#include "../../src/navi48-bringup/src/dcn/navi48_dmubcmd.h" /* 0.0.625: dmubsend / dmubmode / dmubctx - the allowlist, the slot specs, the argument and result layouts, one source of truth with the kext */
#include "../../src/navi48-bringup/src/dcn/navi48_disp2.h" /* 0.0.631: disp2 - the ops, the statuses, the result layouts and the status register list, one source of truth with the kext */
#include "../../src/navi48-bringup/src/Navi48DisplayOps.h" /* 0.0.652 (M5): fbpublish - the status codes and their texts, one source of truth with the kext and the aux framebuffer */
#include "../../src/navi48-bringup/src/Navi48AppKey.h" /* 0.0.640: appallow - the name -> key function and the argument packing, one source of truth with the kext */

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


// ---- 0.0.622 (multi-monitor stage M1): ddcread / dmubring / dispcensus. One IOConnect call per page; the layouts are navi48_dispread.h's (shared with the kext). -----------------------------
static int dr_call(uint64_t action, uint64_t arg, uint64_t out[16]) {
    uint64_t in2[2] = { action, arg };
    uint32_t outCnt = 16;
    for (int i = 0; i < 16; i++) out[i] = 0;
    kern_return_t kr = IOConnectCallScalarMethod(conn, kNavi48SelAccelExperiment, in2, 2, out, &outCnt);
    if (kr != KERN_SUCCESS) {
        printf("accel: call FAILED (0x%x)%s\n", kr, kr == kIOReturnBadArgument ? " - bad argument, OR the verb is not admitted: it needs boot-arg navi48-metal-disp=1, and a kext/CLI pair that both know verbs 91..99" : "");
        return -1;
    }
    return 0;
}

static int cmd_ddcread(const char *a1, const char *a2) {
    if (!a1 || !a2) { fprintf(stderr, "usage: accel ddcread <line 2|3> <block 0..3>   (read-only EDID block over the HDMI DDC line)\n"); return 2; }
    const uint32_t line = (uint32_t)strtoul(a1, NULL, 0), block = (uint32_t)strtoul(a2, NULL, 0);
    if (!n48dr_ddc_line_ok(line) || !n48dr_ddc_block_ok(block)) { fprintf(stderr, "accel ddcread: line must be 2 or 3 and block 0..3 (the kext refuses anything else)\n"); return 2; }
    uint64_t o0[16], o1[16];
    if (dr_call(N48DR_ACT_DDCREAD, n48dr_ddc_arg(line, block, 0), o0) != 0) return 1;
    const uint64_t *v0 = o0 + 3;
    const uint32_t st = (uint32_t)(v0[0] & 0xFF);
    printf("accel ddcread: line %u block %u\n", line, block);
    printf("  status                  : %u (%s)\n", st, n48dr_status_name(st));
    printf("  arbitration last read   : %#010x ; after release %#010x (owner field %u: 0 = given back)\n", (unsigned)(v0[1] & 0xFFFFFFFFu), (unsigned)(v0[0] >> 32), (unsigned)((v0[0] >> 34) & 3u));
    printf("  DC_I2C_SW_STATUS        : %#010x ; released = %u ; read #%u\n", (unsigned)(v0[1] >> 32), (unsigned)((v0[0] >> 18) & 1u), (unsigned)((v0[0] >> 24) & 0xFFu));
    if (st != N48DR_OK) return 3;
    uint8_t edid[N48DR_EDID_BLOCK];
    n48dr_ddc_extract(v0, 0, edid);
    if (dr_call(N48DR_ACT_DDCREAD, n48dr_ddc_arg(line, block, 1), o1) != 0) return 1;
    const uint64_t *v1 = o1 + 3;
    if ((v1[0] & 0xFF) != N48DR_OK) { printf("  page 2 (bytes 88..127)  : status %u (%s)\n", (unsigned)(v1[0] & 0xFF), n48dr_status_name((uint32_t)(v1[0] & 0xFF))); return 3; }
    if (((v1[0] >> 24) & 0xFF) != ((v0[0] >> 24) & 0xFF)) { printf("  page 2 belongs to another read (seq %u vs %u): run again\n", (unsigned)((v1[0] >> 24) & 0xFF), (unsigned)((v0[0] >> 24) & 0xFF)); return 3; }
    n48dr_ddc_extract(v1, 1, edid);
    for (unsigned row = 0; row < 8; row++) {
        printf("  %03x:", row * 16);
        for (unsigned c = 0; c < 16; c++) printf(" %02x", edid[row * 16 + c]);
        printf("\n");
    }
    const uint32_t sum = n48dr_edid_sum(edid);
    printf("  checksum                : %s (sum of the 128 bytes mod 256 = %u; the kext says %s)\n", sum == 0 ? "OK" : "BAD", sum, ((v0[0] >> 16) & 1) ? "OK" : "BAD");
    if (block == 0)
        printf("  EDID header             : %s%s\n", n48dr_edid_header_ok(edid) ? "OK (00 ff ff ff ff ff ff 00)" : "BAD", ((v0[0] >> 17) & 1) == (uint64_t)(n48dr_edid_header_ok(edid) ? 1 : 0) ? "" : " (kext and CLI disagree!)");
    else
        printf("  block tag (byte 0)      : %#04x%s\n", edid[0], edid[0] == 0x02 ? " (CTA-861 extension)" : edid[0] == 0x70 ? " (DisplayID extension)" : "");
    return sum == 0 ? 0 : 3;
}

// 0.0.633: scdcread <line 2|3> <off 0..0x5f> [len 1..16] - READ-ONLY SCDC status bytes from the HDMI sink (slave 0x54) over the same DC_I2C engine; decodes the registers that say whether the sink locks onto our TMDS.
static int cmd_scdcread(const char *a1, const char *a2, const char *a3) {
    if (!a1 || !a2) { fprintf(stderr, "usage: accel scdcread <line 2|3> <offset 0..0x5f> [len 1..16]   (read-only SCDC bytes from the HDMI sink; offset + len <= 0x60; 0x40 = status flags, 0x50..0x56 = error counters)\n"); return 2; }
    const uint32_t line = (uint32_t)strtoul(a1, NULL, 0), off = (uint32_t)strtoul(a2, NULL, 0), len = a3 ? (uint32_t)strtoul(a3, NULL, 0) : 1u;
    if (!n48dr_ddc_line_ok(line) || !n48dr_scdc_range_ok(off, len)) { fprintf(stderr, "accel scdcread: line must be 2 or 3, offset 0..0x5f, len 1..16 and offset + len <= 0x60 (the kext refuses anything else)\n"); return 2; }
    uint64_t o[16];
    if (dr_call(N48DR_ACT_SCDCREAD, n48dr_scdc_arg(line, off, len), o) != 0) return 1;
    const uint64_t *v = o + 3;
    const uint32_t st = (uint32_t)(v[0] & 0xFF);
    printf("accel scdcread: line %u offset %#04x len %u\n", line, off, len);
    printf("  status                  : %u (%s)\n", st, n48dr_status_name(st));
    printf("  arbitration last read   : %#010x ; after release %#010x (owner field %u: 0 = given back)\n", (unsigned)(v[1] & 0xFFFFFFFFu), (unsigned)(v[0] >> 32), (unsigned)((v[0] >> 34) & 3u));
    printf("  DC_I2C_SW_STATUS        : %#010x ; released = %u ; read #%u\n", (unsigned)(v[1] >> 32), (unsigned)((v[0] >> 16) & 1u), (unsigned)((v[0] >> 24) & 0xFFu));
    if (st != N48DR_OK) return 3;
    uint8_t b[N48DR_SCDC_LEN_MAX];
    n48dr_scdc_extract(v, b);
    printf("  %#04x:", off);
    for (uint32_t i = 0; i < len; i++) printf(" %02x", b[i]);
    printf("\n");
    for (uint32_t i = 0; i < len; i++) {
        const uint32_t a = off + i;
        if (a == N48DR_SCDC_SINK_VERSION) printf("  0x01 Sink_Version       : %u\n", b[i]);
        else if (a == N48DR_SCDC_TMDS_CONFIG) printf("  0x20 TMDS_Config        : %#04x  scrambling %s, TMDS bit clock ratio %s\n", b[i], n48dr_scdc_scrambling_enabled(b[i]) ? "ENABLED" : "off", n48dr_scdc_ratio_by_40(b[i]) ? "1/40" : "1/10");
        else if (a == N48DR_SCDC_SCRAMBLER_STATUS) printf("  0x21 Scrambler_Status   : %#04x  sink scrambling %s\n", b[i], n48dr_scdc_scrambling_status(b[i]) ? "ON" : "off");
        else if (a == N48DR_SCDC_STATUS_FLAGS)
            printf("  0x40 Status_Flags_0     : %#04x  clock %s, ch0 %s, ch1 %s, ch2 %s%s\n", b[i], n48dr_scdc_clock_detected(b[i]) ? "DETECTED" : "not detected", n48dr_scdc_ch_locked(b[i], 0) ? "LOCKED" : "not locked",
                   n48dr_scdc_ch_locked(b[i], 1) ? "LOCKED" : "not locked", n48dr_scdc_ch_locked(b[i], 2) ? "LOCKED" : "not locked",
                   n48dr_scdc_locked(b[i]) ? "  => the sink LOCKS onto our TMDS" : "  => the sink does NOT lock on all three channels");
    }
    for (uint32_t ch = 0; ch < 3u; ch++) {      // 0x50/0x51, 0x52/0x53, 0x54/0x55: character error counts (15 bits, valid bit in the high byte)
        const uint32_t lo = N48DR_SCDC_ERR_DETECT + 2u * ch;
        if (lo >= off && lo + 1u < off + len) {
            int valid = 0;
            const uint32_t n = n48dr_scdc_err_count(b[lo - off], b[lo + 1u - off], &valid);
            printf("  %#04x Ch%u error count   : %u (%s)\n", lo, ch, n, valid ? "valid" : "not valid");
        }
    }
    if (N48DR_SCDC_ERR_DETECT + 6u >= off && N48DR_SCDC_ERR_DETECT + 6u < off + len) printf("  0x56 Err_Det_Checksum   : %#04x\n", b[N48DR_SCDC_ERR_DETECT + 6u - off]);
    return 0;
}

static int cmd_dmubring(const char *a1) {
    uint64_t o[16];
    const uint64_t pg = a1 ? strtoull(a1, NULL, 0) : 0;
    if (!n48dr_dmub_arg_ok(pg)) { fprintf(stderr, "usage: accel dmubring [0 summary | 1..25 one command | 0x80 SCRATCH bank]   (READ-ONLY; no DMUB command is ever sent)\n"); return 2; }
    if (dr_call(N48DR_ACT_DMUBRING, 0, o) != 0) return 1;
    const uint64_t *v = o + 3;
    if (pg == N48DR_DMUB_PAGE_SCRATCH) {
        if (dr_call(N48DR_ACT_DMUBRING, pg, o) != 0) return 1;
        v = o + 3;
        printf("accel dmubring: DMCUB_SCRATCH bank (status %u)\n", (unsigned)(v[0] & 0xFF));
        for (unsigned i = 0; i < N48DR_DMUB_SCRATCH_COUNT; i++) printf("  SCRATCH%-2u (dword %#06x) = %#010x\n", i, N48DR_DMUB_SCRATCH_FIRST + i, (unsigned)(v[1 + i / 2] >> (32 * (i & 1))));
        return 0;
    }
    const uint32_t st = (uint32_t)(v[0] & 0xFF), flags = (uint32_t)((v[0] >> 24) & 0xFF), ncmd = (uint32_t)(v[0] >> 32);
    static const char *vn[] = { "ALIVE_IDLE", "ALIVE_BUSY", "NOT_RUNNING", "DMCUB_SOFT_RESET set", "NOT_READY", "RING_INSANE" };
    static const char *mn[] = { "UNKNOWN", "CW4", "REGION4" };
    printf("accel dmubring: summary\n");
    printf("  status                  : %u (%s)\n", st, n48dr_status_name(st));
    printf("  firmware state          : verdict %u (%s), enabled %u, soft_reset %u, dal_fw %u (0 = the VBIOS-loaded firmware), mailbox_rdy %u\n", (unsigned)((v[0] >> 8) & 0xFF),
           ((v[0] >> 8) & 0xFF) < 6 ? vn[(v[0] >> 8) & 0xFF] : "?", flags & 1, (flags >> 1) & 1, (flags >> 2) & 1, (flags >> 3) & 1);
    printf("  DMCUB_CNTL / CNTL2      : %#010x / %#010x ; SEC_CNTL %#010x ; SCRATCH0 (boot status) %#010x\n", (unsigned)v[1], (unsigned)(v[1] >> 32), (unsigned)v[2], (unsigned)(v[2] >> 32));
    printf("  SCRATCH7 / 14 / 15      : %#010x / %#010x / %#010x ; fault addr %#010x\n", (unsigned)v[10], (unsigned)(v[10] >> 32), (unsigned)v[11], (unsigned)(v[11] >> 32));
    printf("  firmware version        : NOT readable here: it is in the fw-meta block in VRAM (below REGION4), reachable only through MM_INDEX, which this verb does not use; GPINT GET_FW_VERSION needs a register write. Use RDNA4FB's rdna4-dmubver=1\n");
    printf("  inbox1                  : base %#010x size %#x WPTR %#x RPTR %#x (%s)\n", (unsigned)v[3], (unsigned)(v[3] >> 32), (unsigned)v[4], (unsigned)(v[4] >> 32), ((flags >> 5) & 1) ? "ring sane" : "ring NOT sane");
    printf("  ring mapping            : %u (%s) ; REGION4_OFFSET %#010x REGION4_OFFSET_HIGH %#010x\n", (unsigned)((v[0] >> 16) & 0xFF), ((v[0] >> 16) & 0xFF) < 3 ? mn[(v[0] >> 16) & 0xFF] : "?", (unsigned)v[5], (unsigned)(v[5] >> 32));
    printf("  ring GPU address        : %#llx ; FB base %#llx ; ring VRAM offset %#llx ; BAR0 aperture %#llx bytes\n", (unsigned long long)v[6], (unsigned long long)v[7], (unsigned long long)v[8], (unsigned long long)v[9]);
    printf("  ring readable via BAR0  : %s ; commands between the ring start and WPTR: %u (decoded: up to %u)\n", ((flags >> 4) & 1) ? "yes" : "NO", ncmd, N48DR_DMUB_MAX_DECODE);
    unsigned first = pg ? (unsigned)pg : 1, last = pg ? (unsigned)pg : ncmd;
    if (!pg && !((flags >> 4) & 1)) { printf("  commands                : not decoded - %s\n", n48dr_status_name(st != N48DR_OK ? st : N48DR_RING_UNREACHABLE)); return st == N48DR_OK ? 0 : 3; }
    for (unsigned p = first; p <= last && p <= N48DR_DMUB_MAX_DECODE; p++) {
        if (dr_call(N48DR_ACT_DMUBRING, p, o) != 0) return 1;
        const uint64_t *c = o + 3;
        const uint32_t cs = (uint32_t)(c[0] & 0xFF);
        if (cs != N48DR_OK) { printf("  [%02u] status %u (%s)\n", p - 1, cs, n48dr_status_name(cs)); if (pg) return 3; break; }
        const uint32_t hdr = (uint32_t)c[1];
        const struct n48dr_dmub_hdr h = n48dr_dmub_decode(hdr);
        printf("  [%02u] header %#010x type %u (%s) sub_type %u (%s) payload_bytes %u%s%s%s%s\n", p - 1, hdr, h.type, n48dr_dmub_type_name(h.type), h.sub_type, n48dr_dmub_subtype_name(h.type, h.sub_type),
               h.payload_bytes, h.ret_status ? " ret_status" : "", h.multi_cmd_pending ? " multi_cmd_pending" : "", h.is_reg_based ? " reg_based" : "", h.reserved_bits_clear ? "" : " RESERVED-BITS-SET");
        printf("       payload[0..31]:");
        for (unsigned i = 0; i < 4; i++) { printf(" "); for (unsigned j = 0; j < 8; j++) printf("%02x", (unsigned)((c[2 + i] >> (8 * j)) & 0xFF)); }
        printf("\n");
    }
    return st == N48DR_OK ? 0 : 3;
}

// 0.0.634 (stage M4a, plane census): print one page of n48dr_census4 / n48dr_census5 by NAME (name, BASE_IDX, absolute BAR5 dword, value); READ-ONLY. Returns 0, or non-zero when the call or the page failed.
static int dc_print_page(uint32_t pg) {
    uint64_t o[16];
    printf("accel dispcensus %u (%s)\n", pg, pg == N48DR_CENSUS4_PAGE ? "the DP watch" : pg >= N48DR_CENSUS7_FIRST_PAGE ? "monitor A instance-1 census page" : "plane page");
    fflush(stdout);       /* a hang on a clock-gated pipe must show which page it was */
    if (dr_call(N48DR_ACT_DISPCENSUS, pg, o) != 0) return 1;
    const uint64_t *v = o + 3;
    const uint32_t st = (uint32_t)(v[0] & 0xFF), n = (uint32_t)((v[0] >> 16) & 0xFF);
    if (st != N48DR_OK) { printf("  page %u: status %u (%s)\n", pg, st, n48dr_status_name(st)); return 3; }
    for (uint32_t i = 0; i < n; i++) {
        const struct n48dr_reg *r = n48dr_census_reg(pg, i);
        printf("  %-52s (BASE_IDX %u, abs %#07x) = %#010x\n", r->name, r->base_idx, n48dr_census_abs(r->base_idx, r->off), (uint32_t)(v[1 + i / 2] >> (32 * (i & 1))));
    }
    fflush(stdout);
    return 0;
}
static int cmd_dispcensus(const char *a1) {
    uint64_t o[16], o2[16];
    if (a1 && !strcmp(a1, "mona")) {      /* 0.0.654: the monitor A (instance 1) census = pages 44..59 in order, READ-ONLY */
        int rc = 0;
        for (uint32_t p = N48DR_CENSUS7_FIRST_PAGE; p < N48DR_CENSUS_ALL_PAGES && rc == 0; p++) rc = dc_print_page(p);
        return rc;
    }
    if (a1 && (!strcmp(a1, "plane") || !strcmp(a1, "dpwatch"))) {      /* 0.0.634: `plane` = pages 10..43 in order (the DP watch first); `dpwatch` = page 10 only */
        int rc = 0;
        const uint32_t last = !strcmp(a1, "dpwatch") ? N48DR_CENSUS4_PAGE : N48DR_CENSUS6_PAGE;
        for (uint32_t p = N48DR_CENSUS4_PAGE; p <= last && rc == 0; p++) rc = dc_print_page(p);
        return rc;
    }
    const uint64_t pg = a1 ? strtoull(a1, NULL, 0) : 0;
    if (!n48dr_census_arg_ok(pg) && a1) { fprintf(stderr, "usage: accel dispcensus [0|1|2..59|plane|dpwatch|mona]   (READ-ONLY; no argument = pages 0 and 1; pages 2..7 = the M4a clock / DIG / PHY census, 0.0.628; pages 8..9 = the M4c substitute registers, 0.0.630; page 10 = the DP watch (`dpwatch`), pages 11..42 = the plane registers of M4a (0.0.634); `plane` = pages 10..43; pages 44..59 = the monitor A instance-1 census (0.0.654), `mona` = pages 44..59)\n"); return 2; }
    if (a1 && pg >= N48DR_CENSUS_PAGES) {      /* 0.0.628 (M4a): one page of the DCCG / DIG / RDPCSTX table; name, absolute BAR5 dword, value */
        if (dr_call(N48DR_ACT_DISPCENSUS, pg, o) != 0) return 1;
        const uint64_t *v = o + 3;
        const uint32_t st = (uint32_t)(v[0] & 0xFF), n = (uint32_t)((v[0] >> 16) & 0xFF);
        printf("accel dispcensus %u (M4a page)\n", (unsigned)pg);
        if (st != N48DR_OK) { printf("  page %u: status %u (%s)\n", (unsigned)pg, st, n48dr_status_name(st)); return 3; }
        for (uint32_t i = 0; i < n; i++) {
            const struct n48dr_reg *r = n48dr_census_reg((uint32_t)pg, i);
            printf("  %-38s (BASE_IDX %u, abs %#07x) = %#010x\n", r->name, r->base_idx, n48dr_census_abs(r->base_idx, r->off), (uint32_t)(v[1 + i / 2] >> (32 * (i & 1))));
        }
        return 0;
    }
    uint32_t vals[N48DR_CENSUS_COUNT];
    uint32_t fc0[4] = { 0 }, fc1[4] = { 0 };
    int bad = 0;
    printf("accel dispcensus\n");
    for (uint32_t page = 0; page < N48DR_CENSUS_PAGES; page++) {
        if (a1 && page != pg) continue;
        if (dr_call(N48DR_ACT_DISPCENSUS, page, o) != 0) return 1;
        const uint64_t *v = o + 3;
        const uint32_t st = (uint32_t)(v[0] & 0xFF), n = (uint32_t)((v[0] >> 16) & 0xFF);
        if (st != N48DR_OK) { printf("  page %u: status %u (%s)\n", page, st, n48dr_status_name(st)); return 3; }
        for (uint32_t i = 0; i < n; i++) vals[n48dr_census_first(page) + i] = (uint32_t)(v[1 + i / 2] >> (32 * (i & 1)));
        for (uint32_t i = 0; i < n; i++) printf("  %-30s (BASE_IDX %u, dword %#06x) = %#010x\n", n48dr_census[n48dr_census_first(page) + i].name, n48dr_census[n48dr_census_first(page) + i].base_idx,
                                               n48dr_census[n48dr_census_first(page) + i].off, vals[n48dr_census_first(page) + i]);
        if (page == 0) for (unsigned k = 0; k < 4; k++) fc0[k] = vals[16 + k];
    }
    if (!a1 || pg == 0) {
        struct timespec ts = { 0, 60 * 1000 * 1000 };   /* 60 ms: three frames at 60 Hz */
        nanosleep(&ts, NULL);
        if (dr_call(N48DR_ACT_DISPCENSUS, 0, o2) != 0) return 1;
        for (unsigned k = 0; k < 4; k++) fc1[k] = (uint32_t)(o2[3 + 1 + (16 + k) / 2] >> (32 * ((16 + k) & 1)));
        printf("  OTG frame counters over 60 ms:");
        for (unsigned k = 0; k < 4; k++) printf(" OTG%u %u -> %u (%s)", k, fc0[k], fc1[k], fc1[k] != fc0[k] ? "COUNTING" : "stopped");
        printf("\n");
    }
    return bad;
}

// ---- 0.0.624 (multi-monitor stage M1.5): region4read / region4dump. READ-ONLY dumps of the DMUB REGION4 window in VRAM (the first 64 KiB of it). One verb call returns 22 dwords, so a read of n dwords is
// ceil(n / 22) calls (page 0 reads the memory, pages 1..2 return the kext's stored rest); the layouts and the window rules are navi48_dispread.h's (shared with the kext). ------------------------------
static int r4_fetch(uint32_t off, uint32_t n, uint32_t *dst, uint64_t *base) {
    uint64_t o[16];
    uint32_t seq0 = 0;
    for (uint32_t pg = 0; pg < n48dr_r4_pages_for(n); pg++) {
        if (dr_call(N48DR_ACT_REGION4READ, n48dr_r4_arg(off, n, pg), o) != 0) return -1;
        const uint64_t *v = o + 3;
        const uint32_t st = (uint32_t)(v[0] & 0xFF);
        if (st != N48DR_OK) {
            printf("accel region4read: +%#x x%u page %u: status %u (%s)\n", off, n, pg, st, n48dr_status_name(st));
            if (st == N48DR_REGION4_BAD) printf("  REGION4 GPU address %#llx ; FB base %#llx ; VRAM size %#llx ; REGION4 enabled %u\n", (unsigned long long)v[1], (unsigned long long)v[2], (unsigned long long)v[3], (unsigned)v[4]);
            return 3;
        }
        if (pg == 0) { seq0 = (uint32_t)((v[0] >> 24) & 0xFF); *base = v[1]; }
        else if (((v[0] >> 24) & 0xFF) != seq0) { printf("accel region4read: page %u belongs to another read (seq %u vs %u): run again\n", pg, (unsigned)((v[0] >> 24) & 0xFF), seq0); return 3; }
        const unsigned want = (n - pg * N48DR_R4_PER_PAGE) < N48DR_R4_PER_PAGE ? (n - pg * N48DR_R4_PER_PAGE) : N48DR_R4_PER_PAGE;
        if (n48dr_r4_extract(v, pg, dst) != want) { printf("accel region4read: page %u carries the wrong dword count\n", pg); return 3; }
    }
    return 0;
}
static void r4_rows(uint32_t off, const uint32_t *d, uint32_t n) {   /* 4 dwords a row, runs of all-zero rows collapsed */
    uint32_t zrun = 0;
    for (uint32_t i = 0; i < n; i += 4) {
        const uint32_t k = n - i < 4 ? n - i : 4;
        uint32_t any = 0;
        for (uint32_t j = 0; j < k; j++) any |= d[i + j];
        if (!any && k == 4 && i + 4 < n) { zrun++; continue; }
        if (zrun) { printf("  ... %u row(s) of zeros\n", zrun); zrun = 0; }
        printf("  +%05x:", off + i * 4);
        for (uint32_t j = 0; j < k; j++) printf(" %08x", d[i + j]);
        printf("\n");
    }
    if (zrun) printf("  ... %u row(s) of zeros\n", zrun);
}
static int cmd_region4read(const char *a1, const char *a2) {
    if (!a1) { fprintf(stderr, "usage: accel region4read <offset> [dwords 1..64, default 16]   (READ-ONLY; offset a multiple of 4 inside the first 64 KiB of the DMUB REGION4 window; offset + dwords*4 <= 0x10000)\n"); return 2; }
    const unsigned long long off = strtoull(a1, NULL, 0), n = a2 ? strtoull(a2, NULL, 0) : 16;
    if (!n48dr_r4_range_ok(off, n)) { fprintf(stderr, "accel region4read: offset must be a multiple of 4, dwords 1..64 and offset + dwords*4 <= 0x10000 (the kext refuses anything else)\n"); return 2; }
    uint32_t d[N48DR_R4_MAX_DWORDS] = { 0 };
    uint64_t base = 0;
    const int rc = r4_fetch((uint32_t)off, (uint32_t)n, d, &base);
    if (rc != 0) return rc;
    printf("accel region4read: window base (VRAM offset) %#llx ; +%#llx x %llu dwords\n", (unsigned long long)base, off, n);
    r4_rows((uint32_t)off, d, (uint32_t)n);
    return 0;
}
static int r4_range(const char *label, uint32_t off, uint32_t n, uint32_t *buf, uint64_t *base) {   /* walks n dwords in 64-dword calls into buf */
    for (uint32_t i = 0; i < n; i += N48DR_R4_MAX_DWORDS) {
        const uint32_t k = n - i < N48DR_R4_MAX_DWORDS ? n - i : N48DR_R4_MAX_DWORDS;
        int rc = r4_fetch(off + i * 4, k, buf + i, base);
        if (rc != 0) { printf("accel region4dump: %s: stopped at +%#x\n", label, off + i * 4); return rc; }
    }
    return 0;
}
static int cmd_region4dump(void) {
    static uint32_t ring[0x2000 / 4], vb[16], mb[128], ctx[0x2000 / 4];
    uint64_t base = 0;
    int rc;
    printf("accel region4dump: READ-ONLY dump of the DMUB REGION4 window (raw; labels and the EDID checksum only)\n");
    if ((rc = r4_range("VBIOS ring state", 0x3100, 16, vb, &base)) != 0) return rc;
    printf("window base (VRAM offset) %#llx\n", (unsigned long long)base);
    printf("VBIOS ring state +0x3100..+0x313f (the VBIOS code reads 0x3104 init flag, 0x3110, 0x3120, 0x311c):\n"); r4_rows(0x3100, vb, 16);
    if ((rc = r4_range("mode block", 0x4c00, 128, mb, &base)) != 0) return rc;
    printf("mode block +0x4c00..+0x4dff:\n"); r4_rows(0x4c00, mb, 128);
    if ((rc = r4_range("display contexts", 0x6000, 0x2000 / 4, ctx, &base)) != 0) return rc;
    for (uint32_t c = 0; c < 4; c++) {
        const uint32_t cb = 0x6000 + c * 0x800;
        const uint32_t *x = ctx + (cb - 0x6000) / 4;
        uint8_t edid[128];
        uint32_t sum = 0, nz = 0;
        for (uint32_t i = 0; i < 128; i++) { edid[i] = (uint8_t)(x[(0x28 + i) / 4] >> (8 * ((0x28 + i) & 3))); sum += edid[i]; nz |= edid[i]; }
        sum &= 0xFF;
        printf("context %u at +%#x:\n", c, cb);
        printf("  +0x04 flags (dword)     : %08x\n", x[0x04 / 4]);
        printf("  +0x08 flags (dword)     : %08x ; +0x09 byte = %02x\n", x[0x08 / 4], (x[0x08 / 4] >> 8) & 0xFF);
        printf("  +0x28 EDID (128 bytes)  : checksum %s (sum of the 128 bytes mod 256 = %u)%s\n", sum == 0 ? "OK" : "BAD", sum, nz ? "" : " ; all bytes zero");
        for (uint32_t row = 0; row < 8; row++) { printf("    %03x:", row * 16); for (uint32_t i = 0; i < 16; i++) printf(" %02x", edid[row * 16 + i]); printf("\n"); }
        printf("  +0x1a0 status (dword)   : %08x ; bit 1 (detected) = %u\n", x[0x1a0 / 4], (x[0x1a0 / 4] >> 1) & 1);
        printf("  +0x24c timing list (raw, +0x24c..+0x2ab):\n"); r4_rows(cb + 0x24c, x + 0x24c / 4, 24);
        printf("  +0x2ac flags (dword)    : %08x\n", x[0x2ac / 4]);
        printf("  +0x2c8 byte (dword raw) : %08x ; byte = %02x\n", x[0x2c8 / 4], x[0x2c8 / 4] & 0xFF);
    }
    if ((rc = r4_range("ring", 0x0, 0x2000 / 4, ring, &base)) != 0) return rc;
    printf("ring +0x0000..+0x1fff (64-byte commands; zero rows collapsed):\n"); r4_rows(0x0, ring, 0x2000 / 4);
    printf("accel region4dump: done (4 ranges: VBIOS ring state, mode block, contexts, ring)\n");
    return 0;
}

// ---- 0.0.625 (multi-monitor stages M2 / M3): dmubsend / dmubmode / dmubctx. The FIRST verbs that SEND to the display firmware. They need boot-arg navi48-metal-disp=1 AND navi48-dmubcmd=1 (default OFF: the kext answers
// "boot-arg navi48-dmubcmd is not 1" and touches nothing). The allowlist, the slot specs and the layouts are navi48_dmubcmd.h's (shared with the kext). The CLI checks the allowlist too, but the KEXT is the gate. ------------------
static const char dm_scripts[] =
    "  M2 - replay ONE command the firmware has already accepted (detect + EDID on the monitor A's context 0x7000); the DP console must not blink:\n"
    "    accel dmubring ; accel dispcensus                   pre-check: WPTR == RPTR == 0x700, OTG0 COUNTING, DMCUB enabled\n"
    "    accel region4read 0x3100 16                         pre-check: +0x3104 = 0, +0x3110 = 0x40, +0x3120 = 0x2000\n"
    "    accel region4read 0x71a0 1                          pre-check: ctx 0x7000 status +0x1a0 = 0x01020006\n"
    "    accel dmubctx 0x7000 status 0                       the marker: clear 'detected' (bit 1)\n"
    "    accel dmubsend detect 0x7000                        slot 04000a80 00007000 at +0x700; WPTR 0x740; RPTR must reach 0x740\n"
    "    accel region4read 0x71a0 1 ; accel dispcensus       pass: status back to 0x01020006 (bit 1 set), EDID +0x7028 unchanged, OTG0 still counting, OTG1-3 stopped\n"
    "    on a failed or timed-out send:  accel dmubctx 0x7000 status 0x01020006   (put the marker back by hand; a stuck RPTR needs a cold power-off)\n"
    "  M3 - light the SINK-B (the user must watch; the DP monitor may go dark; rollback below):\n"
    "    accel dmubmode save                                 copy +0x4c00..+0x4c1f into the kext (required before any mode write)\n"
    "    accel dmubmode 0x1a6                                write the 1080p block (04380780 00020780 41a60001 08081008 00000008 3a02007f 0 0), read back\n"
    "    accel dmubsend begin                                08000680 d1=0 d2=0x101 at +0x740\n"
    "    accel dmubsend setmode 0x7000                       08000580 d1=0x4c00 d2=0x7000 at +0x780\n"
    "    accel region4read 0x4c08 1                          pass ONLY if (dword & 0x00002000) == 0  (mode block byte +0x09 bit 0x20 clear); if set STOP, skip enable, roll back\n"
    "    accel dmubsend enable 0x7000                        04000780 d1=0x7000 at +0x7c0\n"
    "    accel dmubsend end                                  08000680 d1=0 d2=0 at +0x800\n"
    "    watch: accel dispcensus (which OTG counts, DIG1/DIG2 back-end enables), the monitor A, the SINK-A\n"
    "  M3 rollback (the GOP's own DP sequence):\n"
    "    accel dmubmode restore    (or: accel dmubmode 0x1d4)   write the DP block back (verify with accel region4read 0x4c00 8)\n"
    "    accel dmubsend begin ; accel dmubsend setmode 0x6800 ; accel dmubsend enable 0x6800 ; accel dmubsend end\n"
    "  replay (M2, byte-exact): accel dmubsend replay 0x100   copies the existing ring slot at offset 0x100 (64-byte aligned, below WPTR, its header and d1 / d2 on the allowlist) byte for byte into the next slot.\n"
    "  sub 8 (disable) is untested on this firmware and is allowed ONLY on 0x7000 / 0x7800, never 0x6800 (the live DP: a console kill switch); dmubctx refuses 0x6800; mode / ctx writes need an idle ring; every send waits at most 100 ms (pclk / phyc / digc / phyd / digd: 2 s) and is never retried; pclk / phyc / digc (0.0.630, 0.0.632) and otg2 / phyd / digd (0.0.633, the monitor B) send fixed mainline VBIOS templates by ID only.\n";
// ---- 0.0.634 (stage M4a item 4): the DP WATCH. READS of the live DP pipe's plane side (0.0.635: FIFTEEN rows; dispcensus page 10 = n48dr_census4 = rows 0..8 of n48d2_dpx_regs and page 43 = n48dr_census6 = rows 10..14 + the HUBP0 primary HIGH
// dword, both pinned equal by tests/native_disp2_test.cpp; row 9 is HUBP0_DCHUBP_CNTL again with the strict mask), read before and after every disp2 op and every dmubsend template. dp_watch_read returns 0 and fills v[] (masked as n48d2_dpx_regs
// says; v[15] = the primary HIGH dword), -1 if a kext call or a page failed. dp_watch_report prints before -> after and returns how many of rows 0..13 changed; row 14 (HUBP0's primary address) is a RULE judged by the KEXT (a legitimate desktop flip moves it
// while the native scanout is acquired), so it is printed, never counted here. HUBP0's underflow bits [30:28] are printed on their own line. Reads only: no write path exists in either function.
static int dp_watch_page(uint32_t page, uint32_t count, uint32_t *raw) {
    uint64_t o[16];
    if (dr_call(N48DR_ACT_DISPCENSUS, page, o) != 0) return -1;
    const uint64_t *w = o + 3;
    if ((uint32_t)(w[0] & 0xFF) != N48DR_OK || (uint32_t)((w[0] >> 16) & 0xFF) != count) return -1;
    for (uint32_t i = 0; i < count; i++) raw[i] = (uint32_t)(w[1 + i / 2] >> (32 * (i & 1)));
    return 0;
}
static int dp_watch_read(uint32_t v[N48D2_DPX_BUF]) {
    uint32_t r4[N48DR_CENSUS4_COUNT], r6[N48DR_CENSUS6_COUNT];
    if (dp_watch_page(N48DR_CENSUS4_PAGE, N48DR_CENSUS4_COUNT, r4) != 0 || dp_watch_page(N48DR_CENSUS6_PAGE, N48DR_CENSUS6_COUNT, r6) != 0) return -1;
    for (uint32_t i = 0; i < N48DR_CENSUS4_COUNT; i++) v[i] = r4[i] & n48d2_dpx_regs[i].mask;
    v[9] = r4[4] & n48d2_dpx_regs[9].mask;                                   /* row 9 = HUBP0_DCHUBP_CNTL again (page 10's row 4), strict mask */
    for (uint32_t i = 0; i < 5; i++) v[10 + i] = r6[i] & n48d2_dpx_regs[10 + i].mask;
    v[N48D2_DPX_BUF - 1u] = r6[5];                                           /* the primary address HIGH dword */
    return 0;
}
static unsigned dp_watch_report(const char *what, const uint32_t a[N48D2_DPX_BUF], const uint32_t b[N48D2_DPX_BUF]) {
    unsigned changed = 0;
    for (uint32_t i = 0; i < N48D2_DPX_REGS; i++) {
        if (i == N48D2_DPX_RULE) {
            printf("  DP %-24s : %#x:%#010x -> %#x:%#010x  (a RULE: not A / B, inside the scanout window; the kext judges it)\n", n48d2_dpx_regs[i].name, a[N48D2_DPX_BUF - 1u], a[i], b[N48D2_DPX_BUF - 1u], b[i]);
            continue;
        }
        const int ch = a[i] != b[i];
        changed += (unsigned)ch;
        printf("  DP %-24s : %#010x -> %#010x%s\n", n48d2_dpx_regs[i].name, a[i], b[i], ch ? "  *** CHANGED ***" : "");
    }
    printf("  DP HUBP0 underflow 30:28 : %u -> %u%s\n", (a[4] & N48D2_HUBP_UNDERFLOW_MASK) >> 28, (b[4] & N48D2_HUBP_UNDERFLOW_MASK) >> 28, (a[4] & N48D2_HUBP_UNDERFLOW_MASK) != (b[4] & N48D2_HUBP_UNDERFLOW_MASK) ? "  *** CHANGED ***" : "");
    if (changed) printf("  *** DP DISTURBED (%s): %u of %u DP plane registers changed; STOP, run the ROLLBACK, do not retry this boot ***\n", what, changed, N48D2_DPX_REGS);
    else printf("  DP plane watch (%s): all %u registers unchanged (row 14 is the kext's rule)\n", what, N48D2_DPX_REGS);
    return changed;
}
static int dm_usage(const char *verb) {
    fprintf(stderr, "usage: accel dmubsend detect <ctx>|begin|end|setmode <ctx>|enable <ctx>|disable <0x7000|0x7800>|replay <slot_off>|pclk otg3-on|otg3-off|otg1-on|otg1-off|otg2-on|otg2-off|phyc enable|disable|phyd enable|disable|digc setup|digd setup|1440 words: pclk otg2-1440-on, phyd enable-1440, digd setup-1440   ctx 0x6800 (DFP2, the live DP), 0x7000 (DFP3, the monitor A) or 0x7800 (DFP4, the monitor B); disable never takes 0x6800\n"
                    "       accel dmubmode save|restore|0x1a6|0x1d4\n"
                    "       accel dmubctx <0x7000|0x7800> status <value>\n"
                    "  needs boot-arg navi48-metal-disp=1 AND navi48-dmubcmd=1. The scripts (%s):\n%s", verb, dm_scripts);
    return 2;
}
static int cmd_dmubsend(const char *a1, const char *a2) {
    uint32_t kind = 0;
    if (a1 && !strcmp(a1, "replay")) {      // 0.0.626 (F8): copy an existing ring slot byte for byte (the kext judges it)
        if (!a2) return dm_usage("dmubsend");
        const unsigned long long off = strtoull(a2, NULL, 0);
        uint32_t srcOff = 0;
        if (off > 0xFFFFull || n48dm_replay_unarg(n48dm_replay_arg((uint32_t)off), &srcOff) != 0u) { fprintf(stderr, "accel dmubsend replay: %s\n", n48dr_status_name(N48DR_REPLAY_REFUSED)); return 2; }
        uint64_t o[16];
        printf("accel dmubsend replay: the ring slot at offset %#x is copied byte for byte into the next slot (the kext requires its header and d1 / d2 on the allowlist and the offset below WPTR)\n", srcOff);
        if (dr_call(N48DM_ACT_SEND, n48dm_replay_arg(srcOff), o) != 0) return 1;
        struct n48dm_send_out r;
        n48dm_send_unpack(o + 3, &r);
        printf("  source slot             : %08x %08x %08x\n", r.hdr, r.d1, r.d2);
        printf("  status                  : %u (%s)\n", r.status, n48dr_status_name(r.status));
        printf("  command sent            : %s\n", r.sent ? "yes (WPTR moved)" : "NO (WPTR was not written)");
        printf("  WPTR                    : %#x -> %#x ; RPTR at the start %#x, at the end %#x (%s)\n", r.old_wptr, r.new_wptr, r.rptr_start, r.rptr_end, r.rptr_end == r.new_wptr ? "reached the new WPTR" : "did NOT reach the new WPTR");
        printf("  polls / elapsed         : %u polls, %u us ; OTG0 frame counter %u -> %u ; VBIOS ring variables %s\n", r.polls, r.elapsed_us, r.frames0, r.frames1, r.vars ? "updated and read back" : "NOT updated");
        if (r.status == N48DR_POLL_TIMEOUT) printf("  *** RPTR is stuck: nothing was retried; do NOT send more; a cold power-off clears the firmware ***\n");
        return r.status == N48DR_OK ? 0 : 3;
    }
    if (a1 && (!strcmp(a1, "pclk") || !strcmp(a1, "phyc") || !strcmp(a1, "digc") || !strcmp(a1, "phyd") || !strcmp(a1, "digd"))) {      // 0.0.630 (M4c): a mainline VBIOS template (sub 2 SET_PIXEL_CLOCK / sub 1 TRANSMITTER_CONTROL; 0.0.632: sub 0 DIGX_ENCODER_CONTROL), sent by template ID; the CLI never builds a dword
        uint32_t id = N48DM_TPL_NONE;
        if (!a2) return dm_usage("dmubsend");
        if (!strcmp(a1, "digc")) {      // 0.0.632: DIGC stream setup (DVI, 4 lanes, 148.5 MHz) exactly as Linux's dcn401 sends it; Linux has NO encoder-control disable on this path, so `disable` has no template and is refused here
            if (!strcmp(a2, "disable")) { fprintf(stderr, "accel dmubsend digc disable: refused - Linux sends no DIGX_ENCODER_CONTROL disable for a TMDS stream on dcn401 (no template exists); use `accel disp2 off` and `accel dmubsend phyc disable`\n"); return 2; }
            id = !strcmp(a2, "setup") ? N48DM_TPL_DIGC_SETUP_DVI : N48DM_TPL_NONE;
        }
        else if (!strcmp(a1, "digd")) {      // 0.0.633: the monitor B's DIGD stream setup (DVI, 4 lanes, 148.5 MHz); like digc it has no disable
            if (!strcmp(a2, "disable")) { fprintf(stderr, "accel dmubsend digd disable: refused - Linux sends no DIGX_ENCODER_CONTROL disable for a TMDS stream on dcn401 (no template exists); use `accel disp2 off 2` and `accel dmubsend phyd disable`\n"); return 2; }
            id = !strcmp(a2, "setup") ? N48DM_TPL_DIGD_SETUP_DVI : !strcmp(a2, "setup-1440") ? N48DM_TPL_DIGD_SETUP_DVI_1440 : N48DM_TPL_NONE;
        }
        else if (!strcmp(a1, "pclk")) id = !strcmp(a2, "otg3-on") ? N48DM_TPL_PCLK_OTG3_ON : !strcmp(a2, "otg3-off") ? N48DM_TPL_PCLK_OTG3_OFF : !strcmp(a2, "otg1-on") ? N48DM_TPL_PCLK_OTG1_ON : !strcmp(a2, "otg1-off") ? N48DM_TPL_PCLK_OTG1_OFF :
                                         !strcmp(a2, "otg2-on") ? N48DM_TPL_PCLK_OTG2_ON : !strcmp(a2, "otg2-off") ? N48DM_TPL_PCLK_OTG2_OFF : !strcmp(a2, "otg2-1440-on") ? N48DM_TPL_PCLK_OTG2_1440_ON : N48DM_TPL_NONE;
        else if (!strcmp(a1, "phyd")) id = !strcmp(a2, "enable") ? N48DM_TPL_PHYD_ENABLE_DVI : !strcmp(a2, "disable") ? N48DM_TPL_PHYD_DISABLE : !strcmp(a2, "enable-1440") ? N48DM_TPL_PHYD_ENABLE_DVI_1440 : N48DM_TPL_NONE;
        else id = !strcmp(a2, "enable") ? N48DM_TPL_PHYC_ENABLE_DVI : !strcmp(a2, "disable") ? N48DM_TPL_PHYC_DISABLE : N48DM_TPL_NONE;
        const struct n48dm_tpl *tp = n48dm_tpl_get(id);
        if (!tp) return dm_usage("dmubsend");
        uint64_t o[16];
        uint32_t dpw0[N48D2_DPX_BUF], dpw1[N48D2_DPX_BUF];
        if (dp_watch_read(dpw0) != 0) { printf("accel dmubsend %s %s: REFUSED by the CLI: the DP watch (dispcensus page 10) could not be read; nothing was sent\n", a1, a2); return 1; }
        printf("accel dmubsend %s %s (template %s): slot %08x %08x %08x %08x %08x (11 zero dwords); waits at most 2 s, never retried\n", a1, a2, tp->name, tp->dw[0], tp->dw[1], tp->dw[2], tp->dw[3], tp->dw[4]);
        if (dr_call(N48DM_ACT_SEND, n48dm_tpl_arg(id), o) != 0) return 1;
        struct n48dm_send_out r;
        n48dm_send_unpack(o + 3, &r);
        if (dp_watch_read(dpw1) != 0) printf("  *** DP watch AFTER the send could not be read (dispcensus page 10): treat the DP as unchecked ***\n");
        else (void)dp_watch_report("dmubsend template", dpw0, dpw1);
        printf("  status                  : %u (%s)\n", r.status, n48dr_status_name(r.status));
        printf("  command sent            : %s\n", r.sent ? "yes (WPTR moved)" : "NO (WPTR was not written)");
        printf("  WPTR                    : %#x -> %#x ; RPTR at the start %#x, at the end %#x (%s)\n", r.old_wptr, r.new_wptr, r.rptr_start, r.rptr_end, r.rptr_end == r.new_wptr ? "reached the new WPTR" : "did NOT reach the new WPTR");
        printf("  polls / elapsed         : %u polls, %u us ; OTG0 frame counter %u -> %u ; VBIOS ring variables %s\n", r.polls, r.elapsed_us, r.frames0, r.frames1, r.vars ? "updated and read back" : "NOT updated");
        if (r.status == N48DR_POLL_TIMEOUT) printf("  *** RPTR is stuck: nothing was retried; do NOT send more; a cold power-off clears the firmware ***\n");
        return r.status == N48DR_OK ? 0 : 3;
    }
    if (a1 && !strcmp(a1, "detect")) kind = N48DM_K_DETECT;
    else if (a1 && !strcmp(a1, "begin")) kind = N48DM_K_BEGIN;
    else if (a1 && !strcmp(a1, "end")) kind = N48DM_K_END;
    else if (a1 && !strcmp(a1, "setmode")) kind = N48DM_K_SETMODE;
    else if (a1 && !strcmp(a1, "enable")) kind = N48DM_K_ENABLE;
    else if (a1 && !strcmp(a1, "disable")) kind = N48DM_K_DISABLE;
    if (!kind) return dm_usage("dmubsend");
    const int needCtx = kind != N48DM_K_BEGIN && kind != N48DM_K_END;
    if (needCtx && !a2) return dm_usage("dmubsend");
    const uint32_t ctx = needCtx ? (uint32_t)strtoull(a2, NULL, 0) : 0u;
    uint32_t h = 0, d1 = 0, d2 = 0;
    if (n48dm_spec(kind, ctx, &h, &d1, &d2) != 0u) return dm_usage("dmubsend");
    const uint32_t pre = n48dm_slot_check(h, d1, d2);
    if (pre != 0u || !n48dm_send_encodable(d1, d2)) { fprintf(stderr, "accel dmubsend: %s (the CLI refuses before calling; the kext checks the same list)\n", n48dr_status_name(pre ? pre : (uint32_t)N48DR_PAYLOAD_REFUSED)); return 2; }
    uint64_t o[16];
    printf("accel dmubsend %s: slot %08x %08x %08x (13 zero dwords)\n", a1, h, d1, d2);
    if (dr_call(N48DM_ACT_SEND, n48dm_send_arg(h, d1, d2), o) != 0) return 1;
    struct n48dm_send_out r;
    n48dm_send_unpack(o + 3, &r);
    printf("  status                  : %u (%s)\n", r.status, n48dr_status_name(r.status));
    printf("  command sent            : %s\n", r.sent ? "yes (WPTR moved)" : "NO (WPTR was not written)");
    printf("  window base (VRAM off)  : %#llx\n", (unsigned long long)r.base);
    printf("  WPTR                    : %#x -> %#x ; RPTR at the start %#x, at the end %#x (%s)\n", r.old_wptr, r.new_wptr, r.rptr_start, r.rptr_end, r.rptr_end == r.new_wptr ? "reached the new WPTR" : "did NOT reach the new WPTR");
    printf("  polls / elapsed         : %u polls (bound %u x %u us = 100 ms), %u us\n", r.polls, N48DM_POLL_MAX, N48DM_POLL_STEP_US, r.elapsed_us);
    printf("  OTG0 frame counter      : %u -> %u (%s)\n", r.frames0, r.frames1, r.frames1 != r.frames0 ? "COUNTING" : "not counting");
    printf("  VBIOS ring variables    : %s (+0x3114 = 0x40, +0x3118 = old RPTR, +0x311c = new WPTR)\n", r.vars ? "updated and read back" : "NOT updated");
    if (r.status == N48DR_POLL_TIMEOUT) printf("  *** RPTR is stuck: nothing was retried and nothing else was written; do NOT send more; a cold power-off clears the firmware ***\n");
    return r.status == N48DR_OK ? 0 : 3;
}
static int cmd_dmubmode(const char *a1) {
    uint64_t op = 0;
    if (a1 && !strcmp(a1, "save")) op = N48DM_MODE_SAVE;
    else if (a1 && !strcmp(a1, "restore")) op = N48DM_MODE_RESTORE;
    else if (a1) op = strtoull(a1, NULL, 0);
    if (!a1 || !n48dm_mode_op_ok(op)) return dm_usage("dmubmode");
    uint64_t o[16];
    if (dr_call(N48DM_ACT_MODE, op, o) != 0) return 1;
    const uint64_t *v = o + 3;
    const uint32_t st = (uint32_t)(v[0] & 0xFF);
    uint32_t blk[N48DM_MODE_DWORDS], before[N48DM_MODE_DWORDS];
    n48dm_dwords_unpack(v, 2u, blk, N48DM_MODE_DWORDS);
    n48dm_dwords_unpack(v, 6u, before, N48DM_MODE_DWORDS);
    printf("accel dmubmode %s: status %u (%s) ; saved copy %s ; %s\n", a1, st, n48dr_status_name(st), (v[0] >> 24) & 1 ? "held" : "none", (v[0] >> 25) & 1 ? "a mode block was written since the last save / restore" : "window block unmodified by us");
    printf("  window base (VRAM off)  : %#llx\n", (unsigned long long)v[1]);
    if (st == N48DR_OK || st == N48DR_READBACK_MISMATCH) {
        printf("  before the op +0x4c00   :"); for (unsigned i = 0; i < N48DM_MODE_DWORDS; i++) printf(" %08x", before[i]); printf("\n");
        printf("  %s:", op == N48DM_MODE_SAVE ? "saved copy              " : "read back               "); for (unsigned i = 0; i < N48DM_MODE_DWORDS; i++) printf(" %08x", blk[i]); printf("\n");
    }
    return st == N48DR_OK ? 0 : 3;
}
static int cmd_dmubctx(const char *a1, const char *a2, const char *a3) {
    if (!a1 || !a2 || strcmp(a2, "status") != 0 || !a3) return dm_usage("dmubctx");
    const unsigned long long ctx = strtoull(a1, NULL, 0), val = strtoull(a3, NULL, 0);
    if (val > 0xFFFFFFFFull || n48dm_ctx_gate(n48dm_ctx_arg((uint32_t)ctx, 0u)) != 0u || ctx > 0xFFFFull) { fprintf(stderr, "accel dmubctx: %s\n", n48dr_status_name(N48DR_CTX_REFUSED)); return 2; }
    uint64_t o[16];
    if (dr_call(N48DM_ACT_CTX, n48dm_ctx_arg((uint32_t)ctx, (uint32_t)val), o) != 0) return 1;
    const uint64_t *v = o + 3;
    const uint32_t st = (uint32_t)(v[0] & 0xFF);
    printf("accel dmubctx %#llx status <- %#010llx: status %u (%s) ; window base %#llx ; the dword was %#010x, reads back %#010x\n", ctx, val, st, n48dr_status_name(st), (unsigned long long)v[1], (unsigned)v[3], (unsigned)(v[3] >> 32));
    return st == N48DR_OK ? 0 : 3;
}

// ---- 0.0.631 (multi-monitor stage M4d): disp2 timing|connect|off|status - the OTG1 -> DIG2 test pattern on the HDMI display PHY C drives. Needs boot-arg navi48-metal-disp=1 AND navi48-disp2=1 (default OFF:
// the kext answers "boot-arg navi48-disp2 is not 1" and touches nothing). The ops, the statuses and the layouts are navi48_disp2.h's (shared with the kext). The KEXT is the gate; the CLI adds a second DP check
// around timing / connect: a `status` before (OTG0 must count) and after (OTG0 must still count and DIG1_DIG_BE_CNTL must be unchanged), and runs `disp2 off` itself if the after-check fails.
// 0.0.633: every disp2 call names an INSTANCE: `accel disp2 <op> [1|2]`, 1 (the default) = the monitor A (OTG1 / DIG2 / PHY C, exactly as before), 2 = the monitor B SINK-C (OTG2 / DIG3 / PHY D). g_d2_inst is set once by cmd_disp2.
static uint32_t g_d2_inst = N48D2_INST_MONA;
/* 0.0.658 Run B: fbhold names its instance EXACTLY (n48d2_arg_ok refuses the default instance 0), so the monitor A's fbhold is packed as 1; every other monitor A op keeps the default-instance packing it was tested with. */
static uint64_t d2_arg(uint32_t op) { return g_d2_inst == N48D2_INST_MONB ? n48d2_arg_i(op, N48D2_INST_MONB) : op == N48D2_OP_FBHOLD ? n48d2_arg_i(op, N48D2_INST_MONA) : n48d2_arg(op); }
static int d2_usage(void) {
    fprintf(stderr, "usage: accel disp2 timing|timing1440|connect|off|status [1|2]   (timing1440 = the monitor B's 2560x1440@60 timing, instance 2 only: `accel disp2 timing1440 2`; instance 1 = the monitor A: OTG1 / DIG2 / PHY C, the default; 2 = the monitor B: OTG2 / DIG3 / PHY D; no argument: print the run scripts)\n"
                    "       accel disp2 plane|show|flipA|flipB|crc|planeoff|planerec 1|2    (0.0.655: the plane ops take the instance, 1 = the MONA SINK-B at 1920x1080 (buffers 127 x 64 KiB, 240-px bars), 2 = the monitor B; planerec prints the record page of the last plane op)\n"
                    "       accel disp2 plane|show|flipA|flipB|crc|planeoff|fbhold 1|2    (0.0.652: fbhold = HOLD the plane for the rest of the boot after a successful show - IRREVERSIBLE; 0.0.658: `fbhold 1` = the MONA, `fbhold 2` = the monitor B; then `accel fbpublish 1|2`. 0.0.635: the monitor B's plane from VRAM, instance 2 ONLY: plane = draw A and B + program HUBP2 / DPP2 / MPCC2; show = DPG2 off; flipA / flipB = flip to buffer A / B; crc = OTG2 CRC; planeoff = the rollback)\n");
    return 2;
}
static void d2_script(void) {
    printf("M4d run script (0.0.631, 0.0.633). Boot-args navi48-metal-disp=1 navi48-dmubcmd=1 navi48-disp2=1. Watch BOTH screens: the DP SINK-A must stay lit and moving; the monitor A SINK-B (instance 1) or the monitor B SINK-C (instance 2) is the target.\n"
    "  INSTANCE 1 (the monitor A, HDMI ddc2/hpd3, UNIPHY_C):\n"
    "  pre   accel disp2 status               OTG0 frames advance across 50 ms; note DIG1_DIG_BE_CNTL; OTG1 stopped; OTG1_PHYPLL source 0; DIG2 FE off\n"
    "  1     accel dmubsend pclk otg1-on      PLL2 -> OTG1 at 148.5 MHz: OTG1_PHYPLL_PIXEL_RATE_CNTL source becomes 2\n"
    "  2     accel dmubsend phyc enable       PHY C, DVI, 4 lanes: the monitor A wakes (HDMI input banner, black); DIG2 BE_CLK_CNTL 0x2812, BE_EN 1\n"
    "  3     accel disp2 status               OTG0 still counting, DIG1_DIG_BE_CNTL as at pre\n"
    "  4     accel disp2 timing               status 0; OTG1 frames advance; DPG1_CONTROL 0x00661001 (colour squares); 15 = the DP was disturbed and the kext ran `off` itself: go to ROLLBACK\n"
    "  5     accel disp2 connect              status 0; SYMCLKC FE_EN 1 / SRC 2 first; DIG2 FE_EN 1, BE_CNTL FE source 0x4, mapper 2: the monitor A shows the colour squares\n"
    "  6     watch the monitor A 10 s; accel disp2 status   OTG1 counting; DIG2_DIG_FIFO_CTRL0 error bits 28-29 zero\n"
    "  7     accel disp2 off                  DPG1 off, DIG2 FE / FIFO off, SYMCLKC FE off, mapper 0, BE FE source 0, OTG1 stopped, clocks off\n"
    "  8     accel dmubsend phyc disable\n"
    "  9     accel dmubsend pclk otg1-off\n"
    "  post  accel disp2 status               OTG1 stopped, DIG2 FE off, mapper 0, OTG0 counting, DIG1_DIG_BE_CNTL and SYMCLKB as at pre\n"
    "  ROLLBACK (at ANY step where OTG0 stops counting, DIG1_DIG_BE_CNTL or SYMCLKB changes, or the DP screen changes): accel disp2 off ; accel dmubsend phyc disable ; accel dmubsend pclk otg1-off ; stop, no retry this boot.\n"
    "  The monitor A with no signal 10 s after `connect` while OTG1 counts: capture `accel disp2 status` and `accel dispcensus 8`, run the ROLLBACK, stop.\n"
    "  INSTANCE 2 (the monitor B, HDMI ddc3/hpd4, UNIPHY_D; 1920x1080@60 CEA VIC 16, which its EDID lists). First `accel scdcread 3 0x01 1` (sink version) works with no video at all.\n"
    "  pre   accel disp2 status 2             OTG0 counting; OTG2 stopped; OTG2_PHYPLL source 0; DIG3 FE off\n"
    "  1     accel dmubsend pclk otg2-on      PLL3 -> OTG2 at 148.5 MHz: OTG2_PHYPLL_PIXEL_RATE_CNTL source becomes 3 (SUSPECTED by analogy; `disp2 timing 2` refuses with status 8 if it is not 3)\n"
    "  2     accel dmubsend phyd enable       PHY D, DVI, 4 lanes, HPD4: DIG3 BE_CLK_CNTL mode 2 + clock, BE_EN 1\n"
    "  3     accel disp2 timing 2 ; accel disp2 connect 2 ; accel dmubsend digd setup (the order of the monitor A's run)\n"
    "  4     accel scdcread 3 0x40 1          Status_Flags_0: clock detected + ch0..ch2 locked means the monitor B locks onto our TMDS (no one needs to watch); accel scdcread 3 0x50 7 = the character error counters\n"
    "  5     accel disp2 status 2            (prints pages 1-3) ; accel disp2 off 2 ; accel dmubsend phyd disable ; accel dmubsend pclk otg2-off\n"
    "  ROLLBACK: accel disp2 off 2 ; accel dmubsend phyd disable ; accel dmubsend pclk otg2-off.\n"
    "  INSTANCE 2 at 2560x1440@60 (0.0.634, the monitor B's CTA DTD 241.50 MHz; the same order as the 1080p run with the three -1440 templates):\n"
    "  pre   accel dispcensus plane > plane-pre.txt   (M4a census, READ ONLY; also `accel dispcensus dpwatch` = the nine DP registers every op below compares)\n"
    "  1     accel dmubsend pclk otg2-1440-on     PLL3 -> OTG2 at 241.5 MHz (template 13)\n"
    "  2     accel dmubsend phyd enable-1440      PHY D, DVI, 4 lanes, HPD4, symclk 241.5 MHz (template 14)\n"
    "  3     accel disp2 timing1440 2 ; accel disp2 connect 2 ; accel dmubsend digd setup-1440   (template 15)\n"
    "  4     read the SCDC lock status exactly as in the 1080p run above ; accel disp2 status 2 ; then accel disp2 off 2 ; accel dmubsend phyd disable ; accel dmubsend pclk otg2-off\n"
    "  EVERY disp2 op and EVERY dmubsend template prints the DP watch before -> after (MPC_OUT0_MUX, MPCC0 TOP / BOT / OPP, HUBP0 DCHUBP_CNTL incl. underflow 30:28, DPPCLK0 DTO, DPPCLK_CTRL bit 0, DET0, COMPBUF; 0.0.635: and ODM0 underflow, DPP_TOP0, MPCC0 UPDATE_LOCK_SEL, OTG2_GLOBAL_CONTROL2, HUBP0's primary address) and says DP DISTURBED on any change.\n"
    "  THE MONB'S PLANE (0.0.635, M4c / M4d; after step 3 above has the monitor B lit with the DPG pattern, boot-args as above; EVERY op is instance 2 and prints the gate values):\n"
    "  p1    accel disp2 plane 2     draws bars A and B (white..black / reversed) in VRAM, verifies 24 points each, programs HUBP2 / DPP2 / MPCC2 (0.0.638: the HUBP clock is a bare enable, four recorded reads - HUBP2_HUBP_CLK_CNTL, DCCG_GATE_DISABLE_CNTL6, DCCG_GATE_DISABLE_CNTL, DOMAIN2_PG_STATUS - ~1 ms later and after the 2-frame wait, no clock-status wait), (0.0.643) takes OTG2's update lock (optc3_lock), writes the address, joins MPCC2 + the mux and unblanks back to back inside it, releases it, and only then gates FLIP_PENDING = 0 and EARLIEST_INUSE = A (no pre-unblank gate: a blanked HUBP never consumes a pending address); a failed WAIT prints its register and last value; any failure runs the rollback itself (which zeroes the address)\n"
    "  p2    accel disp2 crc 2       CRC with the DPG pattern on (repeat 10x: stable); accel disp2 show 2   DPG2 off: the monitor B shows eight colour bars, white on the left (buffer A) ; accel disp2 crc 2 (repeat: stable, differs from the DPG's)\n"
    "  p3    accel disp2 flipB 2 ; accel disp2 crc 2 ; accel disp2 flipA 2 ; accel disp2 crc 2   (B's bars are reversed; the CRC returns to A's); repeat the cycle 5 times\n"
    "  p4    accel scdcread 3 0x40 (0x4F expected) ; accel disp2 status 2 ; accel disp2 planeoff 2 (the rollback: DPG2 on, blank, mux / MPCC2 none, clocks off, then the buffers are freed ONLY if HUBP2's clock reads off)\n"
    "  p5    (0.0.652, M5; boot-args as above PLUS navi48-fb2=1 and NO navi48-metal-ws; the aux kext com.navi48.accelprobe 0.0.4 installed at its NEW path, tools/native/navi48accel/INSTALL.md)  after p1, p2 and `accel disp2 show 2` (the plane SHOWN on buffer A, p3 may be skipped):\n"
    "        accel disp2 fbhold 2    HOLDS the plane for the rest of the boot (IRREVERSIBLE: the pair is pinned, plane / show / flip / planeoff / timing / connect / off are REFUSED, `crc` and the status pages still work) ; accel disp2 status 2   reports it ; accel fbpublish 2   re-reads the gates and the monitor B's EDID, publishes the display nub: `ioreg -c IOFramebuffer -l` shows TWO framebuffers. A reboot is the only way back. (0.0.658: the same for the monitor A with `fbhold 1` / `fbpublish 1`, see Run B below.)\n"
    "  `accel disp2 off 2` is REFUSED while HUBP2's clock is on: run planeoff first (0.0.636: so are timing, connect and timing1440 on instance 2). planeoff on a plane that is already off answers OK (nothing to do), so a runner may always call it.\n");
    printf("monitor A plane run (0.0.655; instance 1, 1920x1080@60; the monitor B SHOWN on its static plane - `plane 2` + `show 2` - with NO `fbhold` and NO `fbpublish` this boot, the DP untouched). Boot-args navi48-metal-disp=1 navi48-dmubcmd=1 navi48-disp2=1. RUN A (unattended; the spec's Run A):\n"
    "  A0    accel disp2 status 2 ; accel disp2 crc 2   (the monitor B's HUBP2 underflow bits and CRC_A = 0x78097625 / 0x4bd14c15 BEFORE any monitor A op; the kext's MONB WATCH compares nine monitor B rows around every monitor A op)\n"
    "  A1    accel dispcensus mona   (before) ; accel dmubsend pclk otg1-on ; accel dmubsend phyc enable ; accel dmubsend digc setup ; accel disp2 timing 1 ; accel disp2 connect 1 ; accel dispcensus mona   (after)\n"
    "  A2    accel disp2 plane 1     draws bars A and B (1920x1080, 240-px bars, white..black / reversed) in two 127 x 64 KiB buffers, verifies 24 points each (rows 0 / 540 / 1079), programs HUBP1 / DPP1 / MPCC1 with the 24 explicit 1080p values (no COPY0), LOCKs OTG1, unblanks, UNLOCKs\n"
    "  A3    accel disp2 crc 1       (DPG on) ; accel disp2 show 1 ; accel disp2 crc 1 x10 (stable; the DIG2 output CRC line is informational (DIG2 CRC not enabled) and is NOT a pass criterion) ; accel disp2 flipB 1 ; accel disp2 flipA 1 (x5) ; accel disp2 planeoff 1\n"
    "  PASS: every step status 0, the monitor B CRC unchanged (CRC_A), `monitor B watch ... unchanged` after every monitor A op, DIG2 FIFO error 0 (the DIG2 output CRC is informational (DIG2 CRC not enabled): never a pass criterion); whether the image shows needs the user (the monitor A has no SCDC). `accel disp2 off 1` is REFUSED while HUBP1's clock is on: run planeoff first.\n");
    printf("Run B (0.0.658; BOTH displays as macOS framebuffers: the monitor B = instance 2 = display index 1 (OTG2, DDC line 3, 2560x1440), the monitor A = instance 1 = display index 2 (OTG1, DDC line 2, 1920x1080); UNATTENDED, 0 users, the DP at the login window, console root, variant variants/configs/stage17-native-1440-metal-disp-amfi-fb2.plist = navi48-native=1 navi48-metal=1 navi48-metal-disp=1 navi48-dmubcmd=1 navi48-disp2=1 navi48-fb2=1, NO navi48-metal-ws, and NO GPU-mode recipe afterwards; the aux kext com.navi48.accelprobe 0.0.6 installed at /Library/Extensions/Navi48Accel-0.0.6.kext, tools/native/navi48accel/INSTALL.md step 4b). ORDER: nothing is published until BOTH planes are HELD, and there is ONE WindowServer restart, after both nubs exist:\n"
    "  B0    the monitor B chain (the F2 order): accel dmubsend pclk otg2-1440-on ; accel disp2 timing1440 2 ; accel dmubsend phyd enable-1440 ; accel dmubsend digd setup-1440 ; accel disp2 connect 2\n"
    "  B1    accel disp2 plane 2 ; accel disp2 crc 2 ; accel disp2 show 2 ; accel disp2 crc 2   (the monitor B shows the static bars A; CRC_A = 0x78097625 / 0x4bd14c15)\n"
    "  B2    the monitor A chain (the Run A order): accel dmubsend pclk otg1-on ; accel dmubsend phyc enable ; accel dmubsend digc setup ; accel disp2 timing 1 ; accel disp2 connect 1 ; accel disp2 plane 1 ; accel disp2 show 1 ; accel disp2 crc 1   (after EVERY monitor A op: `monitor B watch ... unchanged` and the monitor B CRC still CRC_A)\n"
    "  B3    accel disp2 fbhold 2 ; accel disp2 fbhold 1   (each IRREVERSIBLE this boot and each reads the gates only; from here NO dmubsend of any kind and no plane / show / flip / planeoff / timing / connect / off on a HELD instance: the kext REFUSES them, the monitor A's pclk otg1 / phyc / digc templates included)\n"
    "  B4    accel fbpublish 2 ; accel fbpublish 1   (each re-reads ITS gates and ITS EDID and publishes ITS nub, Navi48DisplayIndex 1 / 2; both must answer status 0; either order works and neither touches the other's state)\n"
    "  B5    ioreg -c IOFramebuffer -l   must show TWO Navi48Framebuffer nodes (one per index) beside RDNA4FB, and an AppleDisplay under each; if WindowServer has not added them: ONE `killall -9 WindowServer` at 0 users (never with a user logged in), then ioreg and `system_profiler SPDisplaysDataType` again (3 displays: the DP, the monitor B, the monitor A)\n"
    "  STOP rule: any non-zero status or exit 3 / 4 / DP DISTURBED BEFORE the holds: accel disp2 planeoff 1 ; accel disp2 off 1 ; accel dmubsend phyc disable ; accel dmubsend pclk otg1-off ; accel disp2 planeoff 2 ; then reboot. AFTER a hold nothing can be undone (a held instance refuses every teardown): reboot.\n");
}
static int d2_status_call(uint32_t r[N48D2_STAT_COUNT], int print) {
    uint64_t o[16];
    if (dr_call(N48D2_ACT, d2_arg(N48D2_OP_STATUS), o) != 0) return -1;
    const uint64_t *v = o + 3;
    const uint32_t st = (uint32_t)(v[0] & 0xFF);
    if (st != N48D2_OK) { printf("accel disp2 status: status %u (%s)\n", st, n48d2_status_name(st)); return -1; }
    for (uint32_t i = 0; i < N48D2_STAT_COUNT; i++) r[i] = (uint32_t)(v[1 + i / 2u] >> (32u * (i & 1u)));
    if (print) {
        if ((v[0] >> N48D2_STAT_HELD_BIT) & 1u) printf("  *** instance %u's plane is HELD for this boot (0.0.652, `disp2 fbhold`): the pair is pinned, plane / show / flip / planeoff / fbhold / timing / connect / off are REFUSED until a reboot ***\n", g_d2_inst);
        const unsigned otg = g_d2_inst, dig = g_d2_inst + 1u;
        const struct n48d2_stat_reg *tbl = n48d2_stat_tbl(g_d2_inst);
        printf("accel disp2 status (instance %u: OTG%u / DIG%u): status 0 (OK)\n", g_d2_inst, otg, dig);
        for (uint32_t i = 0; i < N48D2_STAT_COUNT; i++) printf("  %-32s %#06x = %#010x\n", tbl[i].name, tbl[i].abs, r[i]);
        printf("  rates (50 ms windows; 60 Hz is ~3 frames): OTG%u ~%u Hz, OTG0 ~%u Hz\n", otg, ((r[3] - r[2]) & 0xFFFFFFu) * 20u, ((r[22] - r[21]) & 0xFFFFFFu) * 20u);
        printf("  OTG%u %s (frames %u -> %u in 50 ms), master %u ; DPG%u %s ; DIG%u FE %s, source OTG%u, mode %u, FIFO %s, FIFO error %u ; BE mode %u, clock %u, enable %u, FE source %#x ; mapper link %u ; OTG%u PHYPLL source %u\n",
               otg, r[3] != r[2] ? "COUNTING" : "stopped", r[2], r[3], r[0] & 1u, otg, (r[9] & 1u) ? "ON" : "off", dig, (r[13] & 1u) ? "ON" : "off", r[11] & 7u, r[12] & 7u, (r[14] & 1u) ? "on" : "off", (r[14] >> 28) & 3u,
               r[16] & 7u, (r[16] >> 4) & 1u, r[17] & 1u, (r[15] >> 8) & 0x7Fu, r[18] & 7u, otg, r[20] & 7u);
        printf("  DP side: OTG0 %s (frames %u -> %u in 50 ms) ; DIG1_DIG_BE_CNTL %#010x\n", r[22] != r[21] ? "COUNTING" : "*** NOT COUNTING ***", r[21], r[22], r[23]);
    }
    return 0;
}
static int d2_status2_call(int print) {      // 0.0.632: the second status page (op 5, reads only): SYMCLK, OTG PIXEL_RATE_CNTL, OTG V_TOTAL_CONTROL, FMT control / clamp, HUBP control
    uint64_t o[16];
    if (dr_call(N48D2_ACT, d2_arg(N48D2_OP_STATUS2), o) != 0) return -1;
    const uint64_t *v = o + 3;
    const uint32_t st = (uint32_t)(v[0] & 0xFF);
    if (st != N48D2_OK) { printf("accel disp2 status (page 2): status %u (%s)\n", st, n48d2_status_name(st)); return -1; }
    if (print) {
        const struct n48d2_stat_reg *tbl = n48d2_stat2_tbl(g_d2_inst);
        printf("accel disp2 status, page 2 (reads only):\n");
        for (uint32_t i = 0; i < N48D2_STAT2_COUNT; i++) printf("  %-32s %#06x = %#010x\n", tbl[i].name, tbl[i].abs, (uint32_t)(v[1 + i / 2u] >> (32u * (i & 1u))));
    }
    return 0;
}
// 0.0.633: the third status page (op 6, reads only): the instance's DIG TMDS / FIFO / CRC registers, DIG1's two for comparison, SYMCLKB (the DP's: it must not change) and the instance's SYMCLK. r may be NULL.
static int d2_status3_call(uint32_t r[N48D2_STAT3_COUNT], int print) {
    uint64_t o[16];
    uint32_t loc[N48D2_STAT3_COUNT];
    if (!r) r = loc;
    if (dr_call(N48D2_ACT, d2_arg(N48D2_OP_STATUS3), o) != 0) return -1;
    const uint64_t *v = o + 3;
    const uint32_t st = (uint32_t)(v[0] & 0xFF);
    if (st != N48D2_OK) { printf("accel disp2 status (page 3): status %u (%s)\n", st, n48d2_status_name(st)); return -1; }
    for (uint32_t i = 0; i < N48D2_STAT3_COUNT; i++) r[i] = (uint32_t)(v[1 + i / 2u] >> (32u * (i & 1u)));
    if (print) {
        const struct n48d2_stat_reg *tbl = n48d2_stat3_tbl(g_d2_inst);
        printf("accel disp2 status, page 3 (reads only):\n");
        for (uint32_t i = 0; i < N48D2_STAT3_COUNT; i++) printf("  %-32s %#06x = %#010x\n", tbl[i].name, tbl[i].abs, r[i]);
        printf("  FIFO_CTRL1 levels %#010x ; HDMI_STATUS %#010x ; TMDS_CTL_BITS %#010x (DP's %#010x) ; SYMCLKB %#010x (the DP's: must equal its value before the op)\n", r[2], r[3], r[6], r[11], r[N48D2_STAT3_SYMCLKB]);
    }
    return 0;
}
// 0.0.635 (M4c / M4d): the gate values an op returns (n48d2_unpack_plane), decoded. Reads only.
static void d2_print_gates(uint32_t op, const struct n48d2_out *r, const uint32_t *g) {
    const uint32_t hc = g[N48D2_GT_HUBP_CNTL], fc = g[N48D2_GT_FLIP_CTL];
    const uint64_t early = ((uint64_t)(g[N48D2_GT_EARLY_HI] & 0xFFFFu) << 32) | g[N48D2_GT_EARLY_LO];
    const uint64_t A = ((uint64_t)g[N48D2_GT_A_HI] << 32) | g[N48D2_GT_A_LO], B = ((uint64_t)g[N48D2_GT_B_HI] << 32) | g[N48D2_GT_B_LO];
    const uint32_t meta = g[N48D2_GT_META];
    const unsigned I = g_d2_inst;      /* 0.0.655: the pipe number of the instance (1 = the monitor A, 2 = the monitor B); its DIG is I + 1 */
    printf("  gates (final reads; 'stale' = ODM%u sticky bits already set before op 8 wrote anything):\n", I);
    printf("    HUBP%u_DCHUBP_CNTL       : %#010x  underflow(30:28) %u  SEG_ALLOC_ERR %u  TIMEOUT(23:20) %#x  BLANK_EN %u  NO_OUTSTANDING_REQ %u  VTG_SEL %u\n", I, hc, (hc & N48D2_HUBP_UNDERFLOW_MASK) >> 28, (hc & N48D2_HUBP_SEG_ALLOC_ERR_MASK) ? 1u : 0u, (hc & N48D2_HUBP_TIMEOUT_MASK) >> 20, hc & 1u, (hc >> 1) & 1u, (hc >> 4) & 0xFu);
    printf("    HUBP%u_HUBP_CLK_CNTL     : %#010x  clock enable %u  status(23:20) %#x\n", I, g[N48D2_GT_HUBP_CLK], g[N48D2_GT_HUBP_CLK] & 1u, (g[N48D2_GT_HUBP_CLK] >> 20) & 0xFu);
    printf("    ODM%u_OPTC_INPUT_GLOBAL  : %#010x  underflow occurred(10) %u  occurred-current(13) %u  (stale %#x)\n", I, g[N48D2_GT_ODM2], (g[N48D2_GT_ODM2] >> 10) & 1u, (g[N48D2_GT_ODM2] >> 13) & 1u, (meta >> 16) & 0x2400u);
    printf("    DCN_VM_FAULT_STATUS     : %#010x%s\n", g[N48D2_GT_VMFAULT], g[N48D2_GT_VMFAULT] ? "  *** VM FAULT ***" : "");
    printf("    DCHUBBUB_DET%u_CTRL      : %#010x  size %u  current %u (3 wanted)\n", I, g[N48D2_GT_DET2], g[N48D2_GT_DET2] & N48D2_DET_SIZE_MASK, (g[N48D2_GT_DET2] & N48D2_DET_CUR_MASK) >> 8);
    printf("    EARLIEST_INUSE          : %#llx  (A %#llx, B %#llx) -> %s\n", (unsigned long long)early, (unsigned long long)A, (unsigned long long)B, A && early == A ? "= A" : B && early == B ? "= B" : "NEITHER A NOR B");
    printf("    FLIP_CONTROL            : %#010x  FLIP_PENDING %u\n", fc, (fc & N48D2_FLIP_PENDING_MASK) ? 1u : 0u);
    printf("    MPCC%u_STATUS            : %#x (idle|busy|disabled = %u|%u|%u)   MPC_OUT%u_MUX %#010x (mux %u)   DIG%u_FIFO_CTRL0 %#010x (error 29:28 %u)\n", I, g[N48D2_GT_MPCC_STATUS], g[N48D2_GT_MPCC_STATUS] & 1u, (g[N48D2_GT_MPCC_STATUS] >> 1) & 1u, (g[N48D2_GT_MPCC_STATUS] >> 2) & 1u, I, g[N48D2_GT_MPC_MUX], g[N48D2_GT_MPC_MUX] & 0xFu, I + 1u, g[N48D2_GT_FIFO], (g[N48D2_GT_FIFO] >> 28) & 3u);
    if (op == N48D2_OP_CRC) printf("    OTG%u CRC                : RG %#010x  B %#010x\n", I, g[N48D2_GT_CRC_RG], g[N48D2_GT_CRC_B]);
    if (I == N48D2_INST_MONA) {       /* 0.0.655: the monitor B watch of an MONA op (META bits 16..24; rows 0 DPPCLK_CTRL bit 6, 1 DPPCLK2_DTO, 2 MPC_OUT2_MUX, 3 MPCC2 TOP/OPP, 4 DET2 current, 5 HUBP2 underflow, 6 EARLIEST_INUSE2, 7 DENTIST_DISPCLK_CNTL, 8 OTG2 counting) */
        const uint32_t w2 = (meta >> 16) & 0x1FFu;
        printf("    monitor B watch (kext)       : %s (changed mask %#x)\n", w2 ? "*** THE MONB WAS DISTURBED ***" : "all nine rows unchanged across the op", w2);
    }
    printf("    plane                   : stage %u (%s), latched buffer %c, buffers %s\n", meta & 0xFFu, (meta & 0xFFu) == N48D2_PL_NONE ? "none" : (meta & 0xFFu) == N48D2_PL_ALLOC ? "allocated" : (meta & 0xFFu) == N48D2_PL_PLANE ? "PLANE up, DPG on" : (meta & 0xFFu) == N48D2_PL_SHOWN ? "SHOWN, DPG off" : (meta & 0xFFu) == N48D2_PL_HELD ? "HELD for this boot (0.0.652: pinned, irreversible; Navi48Framebuffer scans A)" : (meta & 0xFFu) == N48D2_PL_LEAKED ? "LEAKED" : "released", ((meta >> 12) & 1u) ? 'B' : 'A', ((meta >> 8) & 1u) ? "RELEASED to the allocator" : (meta & 0xFFu) == N48D2_PL_LEAKED ? "LEAKED (the HUBP clock did not read off or NO_OUTSTANDING_REQ timed out: reboot)" : "kept");
    (void)r;
}
// 0.0.638: the RECORD page of the last plane op (op 13 `planerec`; the kext reads NO register for it): the recorded reads of op 8 (no gate) and the register + LAST value of every WAIT that timed out. Returns 0 and prints, or -1.
static int d2_print_rec(void) {
    uint64_t o[16];
    uint32_t rec[N48D2_REC_N];
    if (dr_call(N48D2_ACT, d2_arg(N48D2_OP_PLANEREC), o) != 0) return -1;
    if ((o[3] & 0xFFu) != N48D2_OK) { printf("  recorded reads          : planerec answered status %u (%s)\n", (unsigned)(o[3] & 0xFFu), n48d2_status_name((uint32_t)(o[3] & 0xFFu))); return -1; }
    n48d2_unpack_rec(o + 3, rec);
    const uint32_t valid = rec[N48D2_RC_VALID];
    printf("  recorded reads (planerec; READS ONLY, never a gate; valid %#x: bit0 sample A, bit1 sample B, bit2 WAIT record, bit3 rollback WAIT record, bit4 DET2 before the unblank, bit5 post-unblank health read, bit6 address left in place):\n", valid);
    printf("                                 HUBP%u_HUBP_CLK_CNTL     DCCG_GATE_DISABLE_CNTL6  DCCG_GATE_DISABLE_CNTL   DOMAIN%u_PG_STATUS\n", g_d2_inst, g_d2_inst);
    for (uint32_t k = 0; k < 2u; k++) {
        const uint32_t *s = &rec[4u * k];
        if (!(valid & (k ? N48D2_RV_B : N48D2_RV_A))) { printf("    %-27s: not taken\n", k ? "after the 2-frame wait" : "~1 ms after the clock enable"); continue; }
        printf("    %-27s: %#010x (enable %u, status 23:20 %#x)  %#010x  %#010x  %#010x\n", k ? "after the 2-frame wait" : "~1 ms after the clock enable", s[0], s[0] & 1u, (s[0] >> 20) & 0xFu, s[1], s[2], s[3]);
    }
    if (valid & N48D2_RV_DET0) printf("    DCHUBBUB_DET%u_CTRL before the unblank: %#010x (current %u)\n", g_d2_inst, rec[N48D2_RC_DET0], (unsigned)((rec[N48D2_RC_DET0] >> 8) & 0xFu));
    if (valid & N48D2_RV_UND) printf("    post-unblank health read (RECORDED, not gated): HUBP%u_DCHUBP_CNTL %#010x (underflow 30:28 %#x, TIMEOUT 23:20 %#x), ODM%u_OPTC_INPUT_GLOBAL_CONTROL %#010x\n", g_d2_inst, rec[N48D2_RC_UND_HUBP], (rec[N48D2_RC_UND_HUBP] >> 28) & 7u, (rec[N48D2_RC_UND_HUBP] >> 20) & 0xFu, g_d2_inst, rec[N48D2_RC_UND_ODM]);
    printf("    plane state             : %s\n", (valid & N48D2_RV_HELD) ? "HELD for this boot (disp2 fbhold ran; irreversible, a reboot releases it)" : "not held");
    if (valid & N48D2_RV_ADDRKEPT) printf("    rollback: NO_OUTSTANDING_REQ never asserted - the primary address was LEFT in place and the buffers are leaked\n");
    if (valid & N48D2_RV_WAIT) printf("    wait timed out at %s (%#06x) last value %#010x\n", n48d2_wait_reg_name(rec[N48D2_RC_WAIT_ABS]), rec[N48D2_RC_WAIT_ABS], rec[N48D2_RC_WAIT_VAL]);
    if (valid & N48D2_RV_RB) printf("    rollback: wait timed out at %s (%#06x) last value %#010x\n", n48d2_wait_reg_name(rec[N48D2_RC_RB_ABS]), rec[N48D2_RC_RB_ABS], rec[N48D2_RC_RB_VAL]);
    return 0;
}
/* 0.0.657: the monitor B watch outcome of an MONA op from the result the kext already returns (no kext change): an monitor A op that ends in N48D2_DP_DISTURBED because of the watch carries (first changed row + 1) in the 4-bit guard field (n48d2_w2_first); status OK means the watch compared equal. Runners grep for CHANGED. */
static void d2_print_monb_watch(uint32_t status, uint32_t guard) {
    static const char *const rows[9] = { "DPPCLK_CTRL bit 6", "DPPCLK2_DTO", "MPC_OUT2_MUX", "MPCC2 TOP/OPP", "DET2 current", "HUBP2 underflow", "EARLIEST_INUSE2", "DENTIST_DISPCLK_CNTL", "OTG2 counting" };
    if (status == N48D2_DP_DISTURBED && guard >= 1u && guard <= 9u) printf("  monitor B watch: CHANGED (row %u, %s; the kext rolled the monitor A op back)\n", guard - 1u, rows[guard - 1u]);
    else if (status == N48D2_DP_DISTURBED) printf("  monitor B watch: unchanged (the DP-side check, not the monitor B watch, ended the op)\n");
    else if (status == N48D2_OK || status == N48D2_WAIT_TIMEOUT) printf("  monitor B watch: unchanged\n");
    else printf("  monitor B watch: not evaluated (status %u, %s: the op did not complete)\n", status, n48d2_status_name(status));
}
static int cmd_disp2_plane(const char *a1, uint32_t op, uint32_t bufIdx) {
    uint32_t pre[N48D2_STAT_COUNT], dpw0[N48D2_DPX_BUF], dpw1[N48D2_DPX_BUF];
    const int haveWatch = dp_watch_read(dpw0) == 0;
    if (op != N48D2_OP_PLANEOFF) {         /* the rollback must work even when these reads fail; everything else is REFUSED by the CLI without them */
        if (!haveWatch) { printf("accel disp2 %s: REFUSED by the CLI: the DP watch (dispcensus pages 10 and 43) could not be read; nothing was sent\n", a1); return 1; }
        if (d2_status_call(pre, 0) != 0) return 1;
        if (pre[N48D2_STAT_OTG0_FC_A] == pre[N48D2_STAT_OTG0_FC_A + 1u]) { printf("accel disp2 %s: REFUSED by the CLI: OTG0's frame counter did not advance in 50 ms (%u); nothing was sent\n", a1, pre[N48D2_STAT_OTG0_FC_A]); return 3; }
    } else if (!haveWatch) printf("  (the DP watch could not be read before planeoff: the plane-side check is skipped)\n");
    uint64_t o[16];
    if (dr_call(N48D2_ACT, d2_arg(op) | ((uint64_t)bufIdx << 16), o) != 0) return 1;
    struct n48d2_out r;
    uint32_t g[N48D2_GATES];
    n48d2_unpack_plane(o + 3, &r, g);
    printf("accel disp2 %s %u: status %u (%s)\n", a1, g_d2_inst, r.status, n48d2_status_name(r.status));
    if (r.status == N48D2_DP_DISTURBED && g_d2_inst == N48D2_INST_MONA && r.guard != 0u) printf("  *** the MONB WATCH tripped (0.0.655): the first changed row is row %u (1 + index) of: 0 DPPCLK_CTRL bit 6, 1 DPPCLK2_DTO, 2 MPC_OUT2_MUX, 3 MPCC2 TOP/OPP, 4 DET2 current, 5 HUBP2 underflow, 6 EARLIEST_INUSE2, 7 DENTIST_DISPCLK_CNTL, 8 OTG2 counting; the kext rolled the monitor A op back ***\n", r.guard - 1u);
    printf("  steps                   : %u written of this op's last list ; wait timeouts %u%s\n", r.done, r.timeouts, r.auto_off ? " ; the ROLLBACK (planeoff list) was run by the kext" : "");
    if (r.fail != 0xFFu) printf("  stopped / refused at    : step %u of the last list\n", r.fail);
    if (r.status == N48D2_GUARD_REFUSED) printf("  guard                   : %s\n", r.guard == N48D2_G_FORBIDDEN ? "a FORBIDDEN register (the DP's path)" : r.guard == N48D2_G_OTHER ? "a register of the OTHER instance" : r.guard == N48D2_G_VALUE ? "a forbidden VALUE" : "an unlisted register");
    if (r.pre_abs) printf("  precheck / gate register: %#06x = %#010x\n", r.pre_abs, r.pre_val);
    if (r.pre_wait) printf("  wait timed out at %s (%#06x) last value %#010x\n", n48d2_wait_reg_name(r.pre_abs), r.pre_abs, r.pre_val);      /* 0.0.638: the gate that failed was a WAIT / a polled gate: its register and the LAST value read (no post-rollback read needed) */
    if (op != N48D2_OP_FBHOLD) {      /* 0.0.652: fbhold reads the gates only (no frame counter is read; the DP watch below is the CLI's own before / after) */
        printf("  OTG0 frames (DP)        : %u -> %u (%s)\n", r.f0a, r.f0b, r.f0b != r.f0a ? "COUNTING" : "*** NOT COUNTING ***");
        printf("  OTG%u frames             : %u -> %u (%s)\n", g_d2_inst, r.f1a, r.f1b, r.f1b != r.f1a ? "counting" : "stopped");
    }
    if (r.dp_changed) printf("  kext DP change mask     : %#x (bits 0..5 the six DP registers, 6 SYMCLKB, 7 OTG0 not counting)  *** DP DISTURBED ***\n", r.dp_changed);
    if (r.dpx_changed) printf("  kext DP-plane mask      : %#x (bit i = row i of the 15-row DP watch below; bit 14 = HUBP0's primary-address rule)  *** DP DISTURBED ***\n", r.dpx_changed);
    d2_print_gates(op, &r, g);
    if (op == N48D2_OP_PLANE || op == N48D2_OP_FBHOLD || r.timeouts != 0u) (void)d2_print_rec();      /* 0.0.638: op 8 always (the recorded reads), any op that timed out a WAIT (its register + last value) */
    int watchBad = 0;
    if (haveWatch) {
        if (dp_watch_read(dpw1) != 0) printf("  *** the DP watch AFTER the op could not be read: treat the DP as unchecked ***\n");
        else if (dp_watch_report(a1, dpw0, dpw1) != 0u) watchBad = 1;
    }
    if (r.dp_changed || r.dpx_changed || r.status == N48D2_DP_DISTURBED) watchBad = 1;
    if (watchBad && op == N48D2_OP_FBHOLD) { printf("  *** DP DISTURBED around `accel disp2 fbhold` (the op reads only and wrote nothing): investigate; `planeoff` is REFUSED once the plane is HELD ***\n"); return 4; }
    if (watchBad && op != N48D2_OP_PLANEOFF) {
        printf("  *** ABORT: DP DISTURBED during `accel disp2 %s`: running `accel disp2 planeoff %u` now ***\n", a1, g_d2_inst);
        uint64_t o2[16];
        if (dr_call(N48D2_ACT, d2_arg(N48D2_OP_PLANEOFF), o2) == 0) { struct n48d2_out r2; uint32_t g2[N48D2_GATES]; n48d2_unpack_plane(o2 + 3, &r2, g2); printf("  planeoff: status %u (%s)\n", r2.status, n48d2_status_name(r2.status)); }
        return 4;
    }
    if (watchBad) return 4;
    if (op == N48D2_OP_CRC && g_d2_inst == N48D2_INST_MONA && r.status == N48D2_OK) {      /* 0.0.655: the DIG2 output CRC as the SECOND WITNESS (status3 page, READS ONLY; the DIG CRC must have been enabled by the firmware: nothing in the spec writes DIG2_DIG_OUTPUT_CRC_CNTL, whose census value is 0x00000100) */
        uint32_t r3[N48D2_STAT3_COUNT];
        if (d2_status3_call(r3, 0) == 0) printf("  DIG2 output CRC (informational (DIG2 CRC not enabled)): CNTL %#010x RESULT %#010x%s\n", r3[8], r3[9], (r3[8] & 1u) ? "  (informational only: NOT a pass criterion)" : "  (CRC_EN is 0: the DIG CRC is not running; informational only, NOT a pass criterion)");
    }
    if (op == N48D2_OP_FBHOLD && r.status == N48D2_OK) printf("  *** the %s's plane (instance %u) is now HELD for the rest of this boot (the pair is pinned; nothing can move or free it). Next: `accel fbpublish %u` (needs boot-arg navi48-fb2=1). A reboot is the only way back. ***\n", g_d2_inst == N48D2_INST_MONA ? "monitor A" : "monitor B", g_d2_inst, g_d2_inst);
    return r.status == N48D2_OK ? 0 : 3;
}
// ---- 0.0.652 (multi-monitor stage M5): `accel fbpublish 2|1` (action 105). After `disp2 plane N`, `show N` and `fbhold N` (the plane HELD, the pair pinned) the kext re-reads the live gates and the display's EDID, builds the IMMUTABLE snapshot
// and publishes Navi48DisplayNub under the GPU's PCI device (0.0.658: ONE nub per display index: `fbpublish 2` = the monitor B = instance 2 = Navi48DisplayIndex 1, `fbpublish 1` = the monitor A = instance 1 = Navi48DisplayIndex 2); the aux kext's Navi48Framebuffer
// (com.navi48.accelprobe 0.0.4; 0.0.6 for the monitor A) then matches it and WindowServer sees another display. Needs boot-args navi48-metal-disp=1
// navi48-disp2=1 navi48-fb2=1 (NOT navi48-metal-ws). NO withdraw: a reboot is the only way back. The statuses and their texts are Navi48DisplayOps.h's (N48_FBP_*).
// ---- 0.0.663 (ReBAR + Stage 2 review S3): `accel vramstat` (action 108). READ-ONLY. Layout (out[3..15] = v[0..12]): v0 status (0 ok, 1 the ladder / allocator is not up), v1 visible total, v2 visible free, v3 hi total, v4 hi free, v5 vramLimit,
// v6 mapped BAR0 bytes, v7 BAR0 phys, v8 VRAM bytes, v9 vramBase, v10 the GMC's CPU-visible size, v11 bit0 hi pool exists, v12 hi pool base. Needs boot-arg navi48-metal-disp=1.
static int cmd_vramstat(const char *a1) {
    if (a1) { fprintf(stderr, "usage: accel vramstat   (no argument; read-only)\n"); return 2; }
    uint64_t o[16];
    if (dr_call(108, 0, o) != 0) return 1;
    const uint64_t *v = o + 3;
    if (v[0] > 1) { printf("accel vramstat: status %llu (refused)\n", (unsigned long long)v[0]); return 1; }
    printf("accel vramstat: BAR0 phys %#llx, mapped %llu MiB (%#llx bytes), vramLimit %llu MiB (%#llx), VRAM %llu MiB, vramBase %llu MiB\n", (unsigned long long)v[7], (unsigned long long)(v[6] >> 20), (unsigned long long)v[6],
           (unsigned long long)(v[5] >> 20), (unsigned long long)v[5], (unsigned long long)(v[8] >> 20), (unsigned long long)(v[9] >> 20));
    {   /* 0.0.664: v11 bit0 hi pool, bit1 BAR0's ReBAR capability found, bits 8..31 its current size in MiB, bits 32..63 its supported-sizes mask (bit k = 1 MiB << k) */
        const uint32_t mask = (uint32_t)(v[11] >> 32);
        if (v[11] & 2u) {
            printf("  ReBAR BAR0  : current %llu MiB, supported-sizes mask %#x:", (unsigned long long)((v[11] >> 8) & 0xFFFFFFu), mask);
            unsigned lowest = 99; for (unsigned k = 0; k < 32; k++) if ((mask >> k) & 1u) { printf(" %llu MiB", (unsigned long long)(1ull << k)); if (lowest == 99) lowest = k; }
            if (lowest != 99) printf("  (smallest %llu MiB = ResizeAppleGpuBars code %u)", (unsigned long long)(1ull << lowest), lowest);
            printf("\n");
        } else printf("  ReBAR BAR0  : capability not found / not readable\n");
    }
    if (v[0] == 1) { printf("  allocator: NOT up (the ladder did not reach the GMC allocator); only the BAR0 fields above are valid\n"); return 1; }
    printf("  visible pool: %llu bytes total (%llu MiB), %llu bytes free (%llu MiB), GMC visible size %llu MiB\n", (unsigned long long)v[1], (unsigned long long)(v[1] >> 20), (unsigned long long)v[2], (unsigned long long)(v[2] >> 20), (unsigned long long)(v[10] >> 20));
    if (v[11] & 1u) printf("  hi pool     : %llu bytes total (%llu MiB), %llu bytes free (%llu MiB), based at vram+%#llx\n", (unsigned long long)v[3], (unsigned long long)(v[3] >> 20), (unsigned long long)v[4], (unsigned long long)(v[4] >> 20), (unsigned long long)v[12]);
    else printf("  hi pool     : not created\n");
    return 0;
}
// ---- 0.0.659 (M6 Stage 1a): `accel m6stat [0|1|2|3]` (action 106). READ-ONLY report of the multi-display routing (boot-arg navi48-m6=1 and navi48-metal-disp=1): the per-pipe submits, the IOSurface IDs the kernel learned (3 per pipe are expected), the
// ambiguous ones, the refused presents and their reasons, the performs completed without a copy (pipes on the monitor A / the monitor B), the vblank stamps per OTG and the AGDC answers. No argument prints all four pages (0.0.660: page 3 = per-pipe last-transaction age and the table's self-healing counters). Layout: native_disp.cpp n48disp_verb.
static int m6stat_page(unsigned page) {
    uint64_t o[16];
    if (dr_call(106, page, o) != 0) return 1;
    const uint64_t *v = o + 3;
    const uint32_t st = (uint32_t)v[0];
    if (st != 0) { printf("accel m6stat %u: status %u (%s)\n", page, st, st == 2 ? "display is OFF: boot-arg navi48-metal-disp is not 1" : "refused"); return 1; }
    const uint64_t f = v[1];
    printf("accel m6stat %u: navi48-m6 latch %s; pipe adopted %s; armed %s; %u pipes recorded; surface table %u IDs\n", page, (f & 1u) ? "ON" : "OFF", (f & 2u) ? "yes" : "no", (f & 4u) ? "yes" : "no", (unsigned)((f >> 8) & 0xFFu), (unsigned)((f >> 16) & 0xFFFFu));
    static const char *const nm[3] = { "instance 0 (DP, OTG0)", "instance 1 (monitor A, OTG1)", "instance 2 (monitor B, OTG2)" };
    if (page == 0) {
        for (int i = 0; i < 3; i++) printf("  %-24s : %llu transactions, %llu surface IDs seen (3 expected)\n", nm[i], (unsigned long long)v[2 + i], (unsigned long long)v[5 + i]);
        printf("  ambiguous IDs (seen on two pipes)      : %llu (0 expected)\n  presents refused by the routing guard   : %llu\n  performs completed without any copy    : %llu (pipes on instance 1 / 2)\n  submits on a pipe with no known framebuffer: %llu (0 expected)\n  DP presents the guard let through      : %llu\n",
               (unsigned long long)v[8], (unsigned long long)v[9], (unsigned long long)v[10], (unsigned long long)v[11], (unsigned long long)v[12]);
    } else if (page == 1) {
        for (int i = 0; i < 3; i++) printf("  %-24s : %llu vblank stamps written, %llu refused, last period %llu ns\n", nm[i], (unsigned long long)v[2 + i], (unsigned long long)v[5 + i], (unsigned long long)v[8 + i]);
        printf("  guard refusals: unknown %llu, ambiguous %llu, other instance %llu, no readable ID %llu\n", (unsigned long long)(v[11] & 0xFFFFu), (unsigned long long)((v[11] >> 16) & 0xFFFFu), (unsigned long long)((v[11] >> 32) & 0xFFFFu), (unsigned long long)((v[11] >> 48) & 0xFFFFu));
        printf("  AGDC: %llu framebuffers in the last 0x980 reply; 0x921 answered for a non-DP endpoint %llu times, 0x711 %llu times; last endpoint dword %llu\n", (unsigned long long)(v[12] & 0xFFu), (unsigned long long)((v[12] >> 8) & 0xFFFFu), (unsigned long long)((v[12] >> 24) & 0xFFFFu), (unsigned long long)((v[12] >> 40) & 0xFFFFFFu));
    } else if (page == 4) {   /* 0.0.662 (Stage 2 item 10): the AGDC replies PER INSTANCE and the endpoint dwords seen */
        for (int i = 0; i < 3; i++) printf("  %-24s : AGDC 0x921 answered %llu times, 0x711 %llu times\n", nm[i], (unsigned long long)v[2 + i], (unsigned long long)v[5 + i]);
        printf("  endpoint dwords seen (bitmask 0x%llx:", (unsigned long long)v[8]);
        for (int e = 0; e < 32; e++) if ((v[8] >> e) & 1ull) printf(" %d%s", e, e == 31 ? "+" : "");
        printf(" - exactly {0 1 2} expected with a three-entry list); nfb of the last 0x980 reply %llu; endpoints outside the list %llu (last endpoint dword %llu)\n", (unsigned long long)v[9], (unsigned long long)v[10], (unsigned long long)v[11]);
    } else if (page == 3) {   /* 0.0.660: the pipes' clocks (K1) and the table's self-healing */
        for (int i = 0; i < 3; i++) {
            if (v[2 + i] == ~0ull) printf("  %-24s : no transaction since boot; %llu submits learned since the last table reset\n", nm[i], (unsigned long long)v[5 + i]);
            else printf("  %-24s : last transaction %llu ms ago; %llu submits learned since the last table reset\n", nm[i], (unsigned long long)v[2 + i], (unsigned long long)v[5 + i]);
        }
        printf("  table: %llu IDs re-learned after their old owner went stale, %llu entries evicted (full table), %llu resets, %llu routed-present sightings\n", (unsigned long long)v[8], (unsigned long long)v[9], (unsigned long long)(v[10] & 0xFFFFu), (unsigned long long)(v[10] >> 16));
        printf("  property: %llu writes, %llu publishes folded into another publisher's; AGDC endpoints outside the 0x980 list: %llu (last endpoint dword %llu)\n", (unsigned long long)(v[11] & 0xFFFFFFFFu), (unsigned long long)(v[11] >> 32), (unsigned long long)(v[12] & 0xFFFFFFFFu), (unsigned long long)(v[12] >> 32));
    } else {
        for (int i = 0; i < 3; i++) { printf("  %-24s : IDs", nm[i]); for (int k = 0; k < 3; k++) { const uint64_t e = v[2 + i * 3 + k]; if (e) printf(" %u%s", (unsigned)(e & 0xFFFFFFFFu), (e >> 32) ? "(AMBIGUOUS)" : ""); else printf(" -"); } printf("\n"); }
        printf("  learns: %llu submits learned, %llu table writes published; declined: txn %llu, pipe mismatch %llu, plane %llu, class %llu, id read %llu; entries evicted %llu, bad instance %llu, busy %llu\n", (unsigned long long)(v[11] & 0xFFFFFFFFu), (unsigned long long)(v[11] >> 32),
               (unsigned long long)(v[12] & 0xFFu), (unsigned long long)((v[12] >> 8) & 0xFFu), (unsigned long long)((v[12] >> 16) & 0xFFu), (unsigned long long)((v[12] >> 24) & 0xFFu), (unsigned long long)((v[12] >> 32) & 0xFFu), (unsigned long long)((v[12] >> 40) & 0xFFu), (unsigned long long)((v[12] >> 48) & 0xFFu), (unsigned long long)((v[12] >> 56) & 0xFFu));
    }
    return 0;
}
// ---- 0.0.661 (M6 Stage 1b) + 0.0.662 (M6 Stage 2): `accel m6xstat [page]` / `accel m6xstat <1|2> <page|all>` (action 107). READ-ONLY report of an HDMI instance's scanout (instance 2 = the monitor B, HUBP2 / OTG2, the default; instance 1 = the monitor A, HUBP1 / OTG1) behind boot-args navi48-m6=1 AND navi48-m6flip=1 (instance 1: and navi48-m6flip1=1).
// ONE argument is a PAGE of the monitor B, exactly as 0.0.661 (the 1b kit parses it); TWO arguments are the instance then the page (or `all`). The action argument is page | instance << 8 (the monitor B is sent as the plain page: an older kernel still understands it). Page 0: acquired / restoring / the counters (presents, latched,
// replaced, refused, reuse_inuse_refused, watchdog restores, restores and failures) / A and B; page 1: the three slots (MC, presents, latches), writer refusals, the last address written; page 2: the LIVE registers the run kit reads every minute
// (HUBP2 underflow bits, ODM2 bit 10, the VM fault register, FLIP_PENDING, the programmed address and EARLIEST_INUSE2, OTG2's frame counter). No argument prints all three pages.
static int m6xstat_page(unsigned inst, unsigned page) {
    uint64_t o[16];
    const unsigned tag = inst == 1u ? 0x10u : 0x20u;
    if (dr_call(107, inst == 1u ? (uint64_t)(page | (1u << 8)) : (uint64_t)page, o) != 0) return 1;
    const uint64_t *v = o + 3;
    const uint64_t f = v[0];
    const int off = inst == 1u ? ((f & 3u) != 3u || !(f & (1u << 11))) : ((f & 3u) != 3u);
    if (inst == 1u) printf("accel m6xstat inst1 page %u: navi48-m6 %s, navi48-m6flip %s, navi48-m6flip1 %s%s\n", page, (f & 1u) ? "ON" : "OFF", (f & 2u) ? "ON" : "OFF", (f & (1u << 11)) ? "ON" : "OFF", off ? "  (instance 1 is OFF: nothing was read)" : "");
    else printf("accel m6xstat %u: navi48-m6 %s, navi48-m6flip %s%s\n", page, (f & 1u) ? "ON" : "OFF", (f & 2u) ? "ON" : "OFF", off ? "  (instance 2 is OFF: nothing was read)" : "");
    if (off) return 0;
    if (page == 0) {
        printf("  instance %u: acquired %s, restoring %s, wantRestore %s, restoreFailed %s, console learned %s; generation %u, acquires %u\n", inst, (f & 4u) ? "YES" : "no", (f & 8u) ? "YES" : "no", (f & 16u) ? "YES" : "no", (f & 32u) ? "YES" : "no", (f & 64u) ? "yes" : "no", (unsigned)(v[1] & 0xFFFFFFFFu), (unsigned)(v[1] >> 32));
        printf("  A (console) 0x%llx  B 0x%llx\n", (unsigned long long)v[2], (unsigned long long)v[3]);
        { const unsigned gh = (unsigned)((f >> 8) & 7u); if (gh) printf("  GPU-HELD: %s (EARLIEST_INUSE%u differing from A is NOT a latch failure while instance %u is acquired)\n", gh <= 3 ? "the hardware fetches client slot" : "EARLIEST_INUSE is not a registered slot", inst, inst); if (gh && gh <= 3) printf("  ... slot 0x%x\n", tag + gh - 1); }
        printf("  presents %llu, latched %llu, replaced %llu, refused %llu, latched-by-poll %llu, REUSE_INUSE_REFUSED %llu\n", (unsigned long long)(v[4] & 0xFFFFFFFFu), (unsigned long long)(v[4] >> 32), (unsigned long long)(v[5] & 0xFFFFFFFFu), (unsigned long long)(v[5] >> 32), (unsigned long long)(v[6] & 0xFFFFFFFFu), (unsigned long long)(v[6] >> 32));
        printf("  watchdog restores %llu, restores %llu (failed %llu); Acquire refused %llu (last reason %llu: 1 busy 2 stage 3 buffers 4 cur 5 window 6 not-counting 7 geometry 8 restore-exact 9 address 10 pending 11 hubp 12 vm-fault 13 watchdog 14 dead read); idle %llu ms; M6 table gen %llu\n", (unsigned long long)(v[7] & 0xFFFFFFFFu), (unsigned long long)(v[7] >> 32), (unsigned long long)v[8] & 0xFFFFFFFFull, (unsigned long long)(v[8] >> 32), (unsigned long long)(v[9] & 0xFFFFFFFFu), (unsigned long long)v[10], (unsigned long long)(v[9] >> 32));
        printf("  last restore %llu us; last address written 0x%llx\n", (unsigned long long)v[11], (unsigned long long)v[12]);
    } else if (page == 1) {
        for (int i = 0; i < 3; i++) printf("  slot 0x%x: MC 0x%llx, presents %llu, latches %llu\n", tag + i, (unsigned long long)v[1 + i], (unsigned long long)(v[4 + i] & 0xFFFFFFFFu), (unsigned long long)(v[4 + i] >> 32));
        printf("  writer refusals %llu, write failures %llu; untagged presents refused %llu; geometry refusals %llu\n", (unsigned long long)(v[7] & 0xFFFFFFFFu), (unsigned long long)(v[7] >> 32), (unsigned long long)(v[8] & 0xFFFFFFFFu), (unsigned long long)(v[8] >> 32));
        printf("  first latch %llu ns, last latch %llu ns; front slot 0x%x, pending slot 0x%x (0xffffffff = none); last address written 0x%llx\n", (unsigned long long)v[9], (unsigned long long)v[10], (unsigned)(v[11] & 0xFFFFFFFFu), (unsigned)(v[11] >> 32), (unsigned long long)v[12]);
    } else {
        const uint32_t hc = (uint32_t)v[1], vmf = (uint32_t)(v[1] >> 32), odm = (uint32_t)v[2], fcr = (uint32_t)v[3];
        printf("  HUBP%u_DCHUBP_CNTL 0x%08x (underflow %u, BLANK_EN %u, VTG_SEL %u); DCN_VM_FAULT_STATUS 0x%08x; ODM%u OPTC_INPUT_GLOBAL_CONTROL 0x%08x (bit 10 %u); OTG%u frame counter %llu\n", inst, hc, (hc >> 28) & 7u, hc & 1u, (hc >> 4) & 0xFu, vmf, inst, odm, (odm >> 10) & 1u, inst, (unsigned long long)(v[2] >> 32));
        printf("  FLIP_CONTROL 0x%08x (FLIP_PENDING %u); viewport 0x%08x; programmed 0x%x:0x%08x; EARLIEST_INUSE%u 0x%x:0x%08x; pitch 0x%x; SURFACE_CONFIG 0x%x; SURFACE_CONTROL 0x%x; VMID_SETTINGS_0 0x%x; OTG%u_CONTROL 0x%08x\n", fcr, (fcr >> 8) & 1u, (unsigned)(v[3] >> 32),
               (unsigned)(v[4] >> 32), (unsigned)v[4], inst, (unsigned)(v[5] >> 32), (unsigned)v[5], (unsigned)v[6], (unsigned)(v[6] >> 32), (unsigned)v[7], (unsigned)(v[7] >> 32), inst, (unsigned)v[8]);
        printf("  disp2 plane state of instance %u: stage %u (6 = HELD), cur %u (0 = A), pair pinned %u; A 0x%llx B 0x%llx\n", inst, (unsigned)(v[9] & 0xFFu), (unsigned)((v[9] >> 8) & 0xFFu), (unsigned)((v[9] >> 16) & 1u), (unsigned long long)v[10], (unsigned long long)v[11]);
    }
    return 0;
}
static int cmd_m6xstat(const char *a1, const char *a2) {
    const char *use = "usage: accel m6xstat [0|1|2]   (the monitor B: 0 state and counters, 1 slots and refusals, 2 the live HUBP2 / OTG2 registers; no argument = all three)\n       accel m6xstat <1|2> <0|1|2|all>   (instance 1 = the monitor A, 2 = the monitor B, then the page)\n";
    if (a2) {   // 0.0.662: instance, then page
        if (strcmp(a1, "1") && strcmp(a1, "2")) { fputs(use, stderr); return 2; }
        const unsigned inst = (unsigned)(a1[0] - '0');
        if (!strcmp(a2, "all")) { int rc = 0; for (unsigned p = 0; p < 3; p++) rc |= m6xstat_page(inst, p); return rc; }
        if (strcmp(a2, "0") && strcmp(a2, "1") && strcmp(a2, "2")) { fputs(use, stderr); return 2; }
        return m6xstat_page(inst, (unsigned)(a2[0] - '0'));
    }
    if (a1 && strcmp(a1, "0") && strcmp(a1, "1") && strcmp(a1, "2")) { fputs(use, stderr); return 2; }
    if (a1) return m6xstat_page(2u, (unsigned)(a1[0] - '0'));      // ONE argument: a page of the monitor B, as 0.0.661
    int rc = 0;
    for (unsigned p = 0; p < 3; p++) rc |= m6xstat_page(2u, p);
    return rc;
}
static int cmd_m6stat(const char *a1) {
    if (a1 && strcmp(a1, "0") && strcmp(a1, "1") && strcmp(a1, "2") && strcmp(a1, "3") && strcmp(a1, "4")) { fprintf(stderr, "usage: accel m6stat [0|1|2|3|4]   (4: the AGDC replies per instance and the endpoint dwords seen; 0: pipes and surface IDs, 1: stamps per OTG, guard reasons and AGDC, 2: the IDs and the learn bookkeeping, 3: each pipe's last-transaction age and the table's self-healing; no argument = all four; read-only; needs boot-args navi48-metal-disp=1 and, for anything but zeros, navi48-m6=1)\n"); return 2; }
    if (a1) return m6stat_page((unsigned)(a1[0] - '0'));
    int rc = 0;
    for (unsigned p = 0; p < 5; p++) rc |= m6stat_page(p);
    return rc;
}

static int cmd_fbpublish(const char *a1) {
    if (!a1 || (strcmp(a1, "2") && strcmp(a1, "1"))) { fprintf(stderr, "usage: accel fbpublish 2|1   (2 = the monitor B, 1 = the monitor A: the disp2 instance; after `accel disp2 plane N` / `show N` / `fbhold N`; boot-args navi48-metal-disp=1 navi48-disp2=1 navi48-fb2=1; NO withdraw: reboot)\n"); return 2; }
    const unsigned inst = (unsigned)(a1[0] - '0');      /* 0.0.658: the instance (2 = the monitor B, display index 1; 1 = the monitor A, display index 2) */
    uint64_t o[16];
    if (dr_call(105, inst, o) != 0) return 1;
    const uint64_t *v = o + 3;
    const uint32_t st = (uint32_t)(v[0] & 0xFFu);
    printf("accel fbpublish %u: status %u (%s)\n", inst, st, n48disp_fbp_name(st));
    if (st == N48_FBP_OK) {
        printf("  nub registry entry ID   : %llu   (Navi48DisplayNub, Navi48DisplayIndex %u, attached to the GPU's IOPCIDevice)\n", (unsigned long long)v[1], (unsigned)((v[6] >> 32) & 0xFFFFu));
        printf("  aperture (CPU)          : %#llx, %llu bytes (BAR0 + buffer A's offset)   MC A %#llx   MC B %#llx\n", (unsigned long long)v[2], (unsigned long long)v[3], (unsigned long long)v[4], (unsigned long long)v[5]);
        printf("  mode                    : %ux%u, refresh %#x (16.16 = %.3f Hz), pixel clock %llu Hz, EDID %u bytes\n", (unsigned)(v[6] & 0xFFFFu), (unsigned)((v[6] >> 16) & 0xFFFFu), (unsigned)(v[7] & 0xFFFFFFFFu), (double)(uint32_t)(v[7] & 0xFFFFFFFFu) / 65536.0, (unsigned long long)v[8], (unsigned)(v[7] >> 32));
    }
    if (st != N48_FBP_NOT_HELD && st != N48_FBP_OFF && st != N48_FBP_BAD_ARG && st != N48_FBP_EXISTS && st != N48_FBP_PIPE_ADOPTED && st != N48_FBP_WS_OPEN && st != N48_FBP_METAL_NUB) {
        const uint64_t early = v[10];
        printf("  live gates              : EARLIEST_INUSE %#llx  HUBP<inst>_DCHUBP_CNTL %#010x  FLIP_CONTROL %#010x  DCN_VM_FAULT_STATUS %#010x  hold verdict %#x (0 = buffer A is the front buffer, healthy)\n", (unsigned long long)early, (unsigned)v[11], (unsigned)(v[11] >> 32), (unsigned)v[12], (unsigned)(v[12] >> 32));
        printf("  EDID / registry / BAR0  : block 0 checksum %u, header %u, block 1 checksum %u ; RDNA4FB EDID,DDC<line> present %u, block 0 equals it %u ; aperture ok %u ; DDC status %u\n", (unsigned)(v[9] & 1u), (unsigned)((v[9] >> 1) & 1u), (unsigned)((v[9] >> 2) & 1u), (unsigned)((v[9] >> 3) & 1u), (unsigned)((v[9] >> 4) & 1u), (unsigned)((v[9] >> 5) & 1u), (unsigned)((v[9] >> 8) & 0xFFu));
    }
    if (st == N48_FBP_OK) printf("  Next (0 users, console root): `ioreg -c IOFramebuffer -l` must show another Navi48Framebuffer (one per display index) and its AppleDisplay (the monitor B: \"SINK-C\"; the monitor A: SINK-B); `system_profiler SPDisplaysDataType` lists it; the WindowServer pid unchanged (or ONE `killall -9 WindowServer` at 0 users after both nubs exist).\n");
    return st == N48_FBP_OK ? 0 : 3;
}
static int cmd_disp2(const char *a1, const char *a2) {
    if (!a1) { d2_script(); return 0; }
    {   /* 0.0.635 (M4c / M4d): the plane ops: `accel disp2 plane|show|flipA|flipB|crc|planeoff|planerec 2` (the monitor B); 0.0.655: ... and `... 1` (the monitor A, 1080p60); 0.0.658: fbhold for either instance too */
        const uint32_t pop = !strcmp(a1, "plane") ? N48D2_OP_PLANE : !strcmp(a1, "show") ? N48D2_OP_SHOW : !strcmp(a1, "flipA") || !strcmp(a1, "flipB") ? N48D2_OP_FLIP : !strcmp(a1, "crc") ? N48D2_OP_CRC : !strcmp(a1, "planeoff") ? N48D2_OP_PLANEOFF : !strcmp(a1, "fbhold") ? N48D2_OP_FBHOLD : !strcmp(a1, "planerec") ? N48D2_OP_PLANEREC : 0u;
        if (pop != 0u) {
            if (!a2 || (strcmp(a2, "1") && strcmp(a2, "2"))) { fprintf(stderr, "accel disp2 %s: the plane ops take the instance: `accel disp2 %s 1` (the monitor A, 1920x1080) or `accel disp2 %s 2` (the monitor B, 2560x1440)%s\n", a1, a1, a1, pop == N48D2_OP_FBHOLD ? "; fbhold names its instance too (0.0.658: 1 = the monitor A, 2 = the monitor B)" : ""); return 2; }
            g_d2_inst = !strcmp(a2, "1") ? N48D2_INST_MONA : N48D2_INST_MONB;
            if (pop == N48D2_OP_PLANEREC) return d2_print_rec() == 0 ? 0 : 3;
            return cmd_disp2_plane(a1, pop, !strcmp(a1, "flipB") ? 1u : 0u);
        }
    }
    const uint32_t op = !strcmp(a1, "timing") ? N48D2_OP_TIMING : !strcmp(a1, "timing1440") ? N48D2_OP_TIMING1440 : !strcmp(a1, "connect") ? N48D2_OP_CONNECT : !strcmp(a1, "off") ? N48D2_OP_OFF : !strcmp(a1, "status") ? N48D2_OP_STATUS : 0u;
    if (op == 0u) return d2_usage();
    if (op == N48D2_OP_TIMING1440 && (!a2 || strcmp(a2, "2"))) { fprintf(stderr, "accel disp2 timing1440: the 2560x1440@60 timing is the monitor B's alone: `accel disp2 timing1440 2`\n"); return 2; }
    if (a2) {      // 0.0.633: the instance
        const unsigned long inst = strtoul(a2, NULL, 0);
        if (strcmp(a2, "1") && strcmp(a2, "2")) return d2_usage();
        g_d2_inst = (uint32_t)inst;
    }
    uint32_t pre[N48D2_STAT_COUNT], post[N48D2_STAT_COUNT], pre3[N48D2_STAT3_COUNT], post3[N48D2_STAT3_COUNT];
    if (op == N48D2_OP_STATUS) return (d2_status_call(pre, 1) == 0 && d2_status2_call(1) == 0 && d2_status3_call(NULL, 1) == 0) ? 0 : 3;     // pages 1, 2 and 3 are printed in order
    const int guarded = op == N48D2_OP_TIMING || op == N48D2_OP_TIMING1440 || op == N48D2_OP_CONNECT;
    uint32_t dpw0[N48D2_DPX_BUF], dpw1[N48D2_DPX_BUF];
    int haveWatch = dp_watch_read(dpw0) == 0;      /* 0.0.634: the DP's plane side before the op (dispcensus page 10) */
    int havePre3 = 0;
    if (guarded) {        // the CLI's own pre-check: OTG0 counting, and SYMCLKB noted
        if (!haveWatch) { printf("accel disp2 %s: REFUSED by the CLI: the DP watch (dispcensus page 10) could not be read; nothing was sent\n", a1); return 1; }
        if (d2_status_call(pre, 0) != 0) return 1;
        if (pre[N48D2_STAT_OTG0_FC_A] == pre[N48D2_STAT_OTG0_FC_A + 1u]) { printf("accel disp2 %s: REFUSED by the CLI: OTG0's frame counter did not advance in 50 ms (%u); nothing was sent\n", a1, pre[N48D2_STAT_OTG0_FC_A]); return 3; }
        if (d2_status3_call(pre3, 0) != 0) return 1;
        havePre3 = 1;
    } else {              // `off` must work even when the reads fail (it is the rollback): the DP checks are best effort there
        havePre3 = d2_status3_call(pre3, 0) == 0;
        if (!haveWatch) printf("  (the DP watch could not be read before `off`: the plane-side check is skipped)\n");
    }
    uint64_t o[16];
    if (dr_call(N48D2_ACT, d2_arg(op), o) != 0) return 1;
    struct n48d2_out r;
    n48d2_unpack(o + 3, &r);
    printf("accel disp2 %s %u: status %u (%s)\n", a1, g_d2_inst, r.status, n48d2_status_name(r.status));
    if (g_d2_inst == N48D2_INST_MONA) d2_print_monb_watch(r.status, r.guard);      /* 0.0.657: timing 1 / connect 1 / off 1 print the monitor B watch outcome too */
    printf("  steps                   : %u of %u written ; wait timeouts %u%s\n", r.done, r.total, r.timeouts, r.auto_off ? " ; `off` was run by the kext (rollback)" : "");
    if (op == N48D2_OP_CONNECT) printf("  FIFO reset (enc35)      : %s\n", n48d2_symclk_report(&r));     // 0.0.632: did the FE symbol clock exist when the FIFO was reset?
    if (r.fail != 0xFFu) printf("  stopped / refused at    : step %u\n", r.fail);
    if (r.status == N48D2_GUARD_REFUSED) printf("  guard                   : %s\n", r.guard == N48D2_G_FORBIDDEN ? "a FORBIDDEN register (the DP's path)" : r.guard == N48D2_G_OTHER ? "a register of the OTHER instance" : r.guard == N48D2_G_VALUE ? "a forbidden VALUE" : "an unlisted register");
    if (r.pre_abs) printf("  precheck                : register %#06x = %#010x\n", r.pre_abs, r.pre_val);
    printf("  OTG0 frames (DP)        : %u -> %u (%s)\n", r.f0a, r.f0b, r.f0b != r.f0a ? "COUNTING" : "*** NOT COUNTING ***");
    printf("  OTG%u frames             : %u -> %u (%s)\n", g_d2_inst, r.f1a, r.f1b, r.f1b != r.f1a ? "counting" : "stopped");
    static const char *const dpn[N48D2_DP_REGS] = { "OTG0_OTG_CONTROL", "DIG1_DIG_FE_CNTL", "DIG1_DIG_BE_CLK_CNTL", "DIG1_DIG_BE_CNTL", "DIG1_DIG_BE_EN_CNTL", "DIG1_STREAM_MAPPER" };
    for (unsigned i = 0; i < N48D2_DP_REGS; i++) printf("  %-23s : %#010x -> %#010x%s\n", dpn[i], r.dp_before[i], r.dp_after[i], r.dp_before[i] != r.dp_after[i] ? "  *** CHANGED ***" : "");
    if (r.dpx_changed) printf("  kext DP-plane change mask : %#x (bit i = row i of the DP watch below)  *** DP DISTURBED ***\n", r.dpx_changed);
    printf("  after                   : OTG%u_CONTROL %#010x ; DPG%u_CONTROL %#010x ; DIG%u FE_EN %#x FE_CLK_CNTL %#010x FIFO_CTRL0 %#010x BE_CNTL %#010x\n", g_d2_inst, r.otg1_ctl, g_d2_inst, r.dpg_ctl, g_d2_inst + 1u, r.fe_en, r.fe_clk, r.fifo, r.be_cntl);
    /* 0.0.634: SYMCLKB before / after for EVERY op (the status3 pair; the old per-op line printed unpacked scalars) and the DP plane watch after the op */
    int watchBad = 0;
    {
        int havePost3 = d2_status3_call(post3, 0) == 0;
        if (havePre3 && havePost3) printf("  SYMCLKB_CLOCK_ENABLE    : %#010x -> %#010x%s\n", pre3[N48D2_STAT3_SYMCLKB], post3[N48D2_STAT3_SYMCLKB], pre3[N48D2_STAT3_SYMCLKB] != post3[N48D2_STAT3_SYMCLKB] ? "  *** CHANGED ***" : "");
        else printf("  SYMCLKB_CLOCK_ENABLE    : not compared (a status3 read failed)\n");
        if (havePre3 && havePost3 && pre3[N48D2_STAT3_SYMCLKB] != post3[N48D2_STAT3_SYMCLKB]) { watchBad = 1; printf("  *** DP DISTURBED (SYMCLKB changed) ***\n"); }
        if (haveWatch) {
            if (dp_watch_read(dpw1) != 0) printf("  *** the DP watch AFTER the op could not be read: treat the DP as unchecked ***\n");
            else if (dp_watch_report(a1, dpw0, dpw1) != 0u) watchBad = 1;
        }
    }
    if (guarded) {        // the CLI's own post-check: OTG0 still counting, DIG1_DIG_BE_CNTL and SYMCLKB unchanged, the DP plane watch unchanged, else `disp2 off` at once
        int bad = d2_status_call(post, 0) != 0;
        if (!bad && (watchBad || r.dpx_changed != 0u)) bad = 1;
        if (!bad && (post[N48D2_STAT_OTG0_FC_A] == post[N48D2_STAT_OTG0_FC_A + 1u] || post[23] != pre[23] || post3[N48D2_STAT3_SYMCLKB] != pre3[N48D2_STAT3_SYMCLKB])) bad = 1;
        if (bad) {
            printf("  *** ABORT: the DP check after %s failed (OTG0 %u -> %u, DIG1_DIG_BE_CNTL %#010x -> %#010x, SYMCLKB %#010x -> %#010x): running `accel disp2 off %u` now; then run `accel dmubsend %s disable` and `accel dmubsend pclk otg%u-off` ***\n",
                   a1, post[N48D2_STAT_OTG0_FC_A], post[N48D2_STAT_OTG0_FC_A + 1u], pre[23], post[23], pre3[N48D2_STAT3_SYMCLKB], post3[N48D2_STAT3_SYMCLKB], g_d2_inst, g_d2_inst == N48D2_INST_MONB ? "phyd" : "phyc", g_d2_inst);
            uint64_t o2[16];
            if (dr_call(N48D2_ACT, d2_arg(N48D2_OP_OFF), o2) == 0) { struct n48d2_out r2; n48d2_unpack(o2 + 3, &r2); printf("  disp2 off: status %u (%s), %u of %u steps\n", r2.status, n48d2_status_name(r2.status), r2.done, r2.total); }
            return 4;
        }
        printf("  CLI DP check            : OTG0 counting (%u -> %u), DIG1_DIG_BE_CNTL unchanged (%#010x), SYMCLKB unchanged (%#010x), DP plane watch unchanged\n", post[N48D2_STAT_OTG0_FC_A], post[N48D2_STAT_OTG0_FC_A + 1u], post[23], post3[N48D2_STAT3_SYMCLKB]);
    }
    if (watchBad && !guarded) return 4;     /* `off` itself disturbed the DP: nothing more can be run safely; the caller must stop */
    return r.status == N48D2_OK ? 0 : 3;
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
    // an internal design note; 7 is 0.0.416, an internal design note; 8 is 0.0.417,
    // an internal design note D7 - the 1920x1080 UNIFORM-probe copy of mode 6's live case).
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
    // 82 - 0.0.417 (an internal design note, D1; section 957). `sdmadcc [0|1|2]`: the SDMA0_DCC_CNTL
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
    // 91..93 - 0.0.622 (multi-monitor stage M1, an internal design note): the display instruments. All need boot-arg navi48-metal-disp=1 (admitted past 82 only then).
    //     ddcread <line 2|3> <block 0..3> : an EDID block over the HDMI DDC line with the DC_I2C engine (writes only that engine's registers, after the arbitration read says it is free)
    //     dmubring [0|1..25|0x80]          : READ-ONLY DMUB state and ring decode (the ring is beyond the BAR0 aperture on this card: reported, not read)
    //     dispcensus [0|1|2..9]            : READ-ONLY census of power gates, DIG, OTG, DCCG / PHY PLL and hot-plug registers
    else if (what && !strcmp(what, "ddcread")) in = 91;
    else if (what && !strcmp(what, "dmubring")) in = 92;
    else if (what && !strcmp(what, "dispcensus")) in = 93;
    // 99 - 0.0.633 (multi-monitor): scdcread <line 2|3> <off 0..0x5f> [len 1..16] : READ-ONLY SCDC status bytes from the HDMI sink (slave 0x54) over the same DC_I2C engine as ddcread (needs boot-arg navi48-metal-disp=1)
    else if (what && !strcmp(what, "scdcread")) in = 99;
    // 94 - 0.0.624 (stage M1.5): READ-ONLY dumps of the DMUB REGION4 window in VRAM (needs the same boot-arg; the window base is computed live from the DMCUB registers; MM_INDEX reads via the kext's existing reader).
    //     region4read <off> [n]   : up to 64 dwords at window offset <off> (4-byte aligned, off + n*4 <= 0x10000)
    //     region4dump             : CLI convenience, repeated 94 calls over 0x3100..0x3140, 0x4c00..0x4e00, 0x6000..0x8000 (four contexts, labelled) and the ring 0x0..0x2000
    else if (what && (!strcmp(what, "region4read") || !strcmp(what, "region4dump"))) in = 94;
    // 95..97 - 0.0.625 (stages M2 / M3): the FIRST verbs that SEND to the display firmware. Need boot-arg navi48-metal-disp=1 AND navi48-dmubcmd=1 (default OFF: the kext refuses and touches nothing).
    //     dmubsend detect <ctx>|begin|end|setmode <ctx>|enable <ctx>|disable <0x7000|0x7800>|replay <slot_off> : ONE allowlisted command appended to the DMUB inbox1 ring; waits at most 100 ms for RPTR; never retried
    //     dmubmode save|restore|0x1a6|0x1d4 : the 32-byte mode block at REGION4 + 0x4c00 (a prior save is required before a mode write)
    //     dmubctx <0x7000|0x7800> status <value> : the M2 marker dword (context + 0x1a0); never the live DP context 0x6800
    //     The M2 / M3 scripts are printed by `accel dmubsend` with no argument.
    else if (what && !strcmp(what, "dmubsend")) in = 95;
    else if (what && !strcmp(what, "dmubmode")) in = 96;
    else if (what && !strcmp(what, "dmubctx")) in = 97;
    // 98 - 0.0.631 (stage M4d): disp2 timing|connect|off|status - the OTG1 -> DIG2 test pattern. Needs boot-arg navi48-metal-disp=1 AND navi48-disp2=1. `accel disp2` alone prints the run script.
    else if (what && !strcmp(what, "disp2")) in = 98;
    // 100..102 - 0.0.623 (GPU-apps G1, an internal design note section 4): hang recovery. They exist ONLY when the PC booted with boot-arg navi48-g1=1 (else
    // 0xe00002c2). Root only (this client). Run with WindowServer on the CPU renderer: hangtest refuses while ANY native session is open (a WindowServer one by name).
    //   hangtest [1]        a kernel-owned VMID-8 test context submits ONE IB that never retires (WAIT_REG_MEM on a dword nobody writes); after 2 s HUNG is latched
    //   hangrecover <0..4>  0 auto (mesreset then remap), 1 release (the CPU writes the awaited value: harness control), 2 mesreset (MES RESET legacy GFX queue),
    //                       3 remap (MES unmap + MQD/ring re-init + map), 4 abandon (close the test context, HUNG stays: reboot). HUNG is cleared ONLY after a probe IB
    //                       retires and reads back. Give the method explicitly.
    //   hangstat [0..8]     0 the summary, 1..8 one attempt each
    else if (what && !strcmp(what, "hangtest")) in = 100;
    else if (what && !strcmp(what, "hangrecover")) in = 101;
    else if (what && !strcmp(what, "hangstat")) in = 102;
    // 103 - 0.0.627 (GPU-apps G2, an internal design note section 4): `sessstat [0..4]`, READ-ONLY. Exists ONLY when the PC booted with boot-arg
    // navi48-multisession=1 (else 0xe00002c2). Page 0..3: native session slot 0..3 (VMID 8..11); page 4 (the default): the global page.
    else if (what && !strcmp(what, "sessstat")) in = 103;
    // 104 - 0.0.640 (GPU-apps G4, an internal design note section 4): `appallow add <name>|remove <name>|list`, the kernel's allow-list of user applications that may open a native
    // GPU session (name = the executable's base name, the first 16 characters; see Navi48AppKey.h for what that is worth). Touches no hardware. Exists ONLY when the PC booted with BOTH
    // boot-args navi48-apps=1 and navi48-multisession=1 (else 0xe00002c2). The list is empty at every boot and holds 32 entries; this verb is the only way to change it.
    else if (what && !strcmp(what, "appallow")) in = 104;
    // 105 - 0.0.652 (multi-monitor stage M5): fbpublish 2 - publish the monitor B's display nub for the aux kext's Navi48Framebuffer (after disp2 plane / show / fbhold 2); 0.0.658: `fbpublish 1` = the monitor A's (display index 2). Needs boot-args navi48-metal-disp=1 navi48-disp2=1 navi48-fb2=1. No withdraw: reboot.
    else if (what && !strcmp(what, "fbpublish")) in = 105;
    // 106 - 0.0.659 (M6 Stage 1a): `m6stat [0|1|2|3]` - READ-ONLY report of the multi-display routing: per pipe transactions, the IOSurface IDs the kernel learned (3 per pipe expected), ambiguous IDs, presents refused by the routing guard, performs completed
    // without a copy, vblank stamps per OTG, the AGDC answers. Same latch as the pipe verbs (boot-arg navi48-metal-disp=1); the counters stay 0 without boot-arg navi48-m6=1. No argument = all three pages.
    else if (what && !strcmp(what, "m6stat")) in = 106;
    // 107 - 0.0.661 (M6 Stage 1b): `m6xstat [0|1|2]` - READ-ONLY report of instance 2's scanout (the monitor B): acquired, slots, EARLIEST_INUSE2, FLIP_PENDING, the counters, the watchdog restores, the live HUBP2 / OTG2 health registers. Needs boot-args navi48-m6=1 AND navi48-m6flip=1.
    else if (what && !strcmp(what, "m6xstat")) in = 107;
    // 108 - 0.0.663 (ReBAR + Stage 2 review S3): `vramstat` - READ-ONLY: the kernel allocator's free visible-VRAM bytes and the visible / hi pool sizes, vramLimit, the mapped BAR0 bytes and BAR0's physical address. No argument. Same latch as the pipe verbs (boot-arg navi48-metal-disp=1).
    else if (what && !strcmp(what, "vramstat")) in = 108;
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
    // notes/an internal review note section 7.1). READ-ONLY: neither this verb nor the
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
                        "dumpib rebaseib enablequeue kickdoorbell ringrefs queuestate opengate neuterpoll chanstate pokecompletion signalcompletion bindchannel schedstate stampstate signalstamp stampgap runcheckts runadvance xlatregs srbmprobe resume pm4powerup setvspace kiqenable kiqstamp kiqchan gfxmap gfxstate sdmamap sdmastate faultclear vmstate vmib ringib pagecopy flushdrop kernsub vmpage renderxlat eopbridge bootchain shadercache vmroots ringmap vmctx rootwrite rearmdrain pairing drain flushhook scanout gfxcensus gfxneuter finishread gfxcapture gfxprobe pipeguard agdc agdchold cqprobe ucprobe fbbench fbwc pipeshim pipemode emcensus routea dcnstate dcnvbl dcnflip dcnmode fbname sdmadcc pipeadopt pipearm pipestat pipestamps pipeshortcut pipeagdc pipevbl pipereload ddcread dmubring dispcensus region4read region4dump dmubsend dmubmode dmubctx disp2 hangtest hangrecover hangstat sessstat scdcread appallow fbpublish m6stat m6xstat vramstat\n");
        return 2;
    }
    const char *verb = in == 108 ? "vramstat" : in == 107 ? "m6xstat" : in == 106 ? "m6stat" : in == 105 ? "fbpublish" : in == 104 ? "appallow" : in == 103 ? "sessstat" : in == 99 ? "scdcread" : in == 102 ? "hangstat" : in == 101 ? "hangrecover" : in == 100 ? "hangtest" : in == 98 ? "disp2" : in == 97 ? "dmubctx" : in == 96 ? "dmubmode" : in == 95 ? "dmubsend" : in == 94 ? "region4read" : in == 93 ? "dispcensus" : in == 92 ? "dmubring" : in == 91 ? "ddcread" : in == 90 ? "pipereload" : in == 89 ? "pipevbl" : in == 88 ? "pipeagdc" : in == 87 ? "pipeshortcut" : in == 86 ? "pipestamps" : in == 85 ? "pipestat" : in == 84 ? "pipearm" : in == 83 ? "pipeadopt" : in == 82 ? "sdmadcc" : in == 81 ? "ucprobe" : in == 80 ? "cqprobe" : in == 79 ? "agdchold" : in == 78 ? "fbname" : in == 77 ? "dcnmode" : in == 76 ? "dcnflip" : in == 75 ? "dcnvbl" : in == 74 ? "dcnstate" : in == 73 ? "routea" : in == 72 ? "emcensus" : in == 71 ? "pipemode" : in == 70 ? "pipeshim" : in == 69 ? "fbwc" : in == 68 ? "fbbench" : in == 67 ? "agdc" : in == 66 ? "pipeguard" : in == 65 ? "gfxprobe" : in == 64 ? "gfxcapture" : in == 63 ? "finishread" : in == 62 ? "gfxneuter" : in == 61 ? "gfxcensus" : in == 60 ? "scanout" : in == 59 ? "flushhook" : in == 58 ? "drain" : in == 57 ? "pairing" : in == 56 ? "rearmdrain" : in == 55 ? "rootwrite" : in == 54 ? "vmctx" : in == 53 ? "ringmap" : in == 52 ? "vmroots" : in == 51 ? "shadercache" : in == 50 ? "bootchain" : in == 49 ? "eopbridge" : in == 48 ? "renderxlat" : in == 47 ? "vmpage" : in == 46 ? "kernsub" : in == 45 ? "flushdrop" : in == 44 ? "pagecopy" : in == 43 ? "ringib" : in == 42 ? "vmib" : in == 41 ? "vmstate" : in == 40 ? "faultclear"
                     : in == 39 ? "sdmastate" : in == 38 ? "sdmamap"
                     : in == 37 ? "gfxstate" : in == 36 ? "gfxmap"
                     : in == 35 ? "kiqchan" : in == 34 ? "kiqstamp" : in == 33 ? "kiqenable" : in == 32 ? "setvspace" : in == 31 ? "pm4powerup" : in == 30 ? "resume" : in == 29 ? "srbmprobe" : in == 28 ? "xlatregs" : in == 27 ? "runadvance" : in == 26 ? "runcheckts" : in == 25 ? "stampgap" : in == 24 ? "signalstamp" : in == 23 ? "stampstate" : in == 22 ? "schedstate" : in == 21 ? "bindchannel" : in == 20 ? "signalcompletion" : in == 19 ? "pokecompletion" : in == 18 ? "chanstate" : in == 17 ? "neuterpoll" : in == 16 ? "opengate" : in == 15 ? "queuestate" : in == 14 ? "ringrefs" : in == 13 ? "kickdoorbell" : in == 12 ? "enablequeue" : in == 11 ? "rebaseib" : in == 10 ? "dumpib" : in == 9 ? "ringhooks" : in == 8 ? "programqueue" : in == 7 ? "dumpring" : in == 6 ? "ringstate" : in == 5 ? "startengines" : in == 4 ? "enablerings" : in == 3 ? "synctables"
                     : in == 2 ? "memenable" : (in ? "fire" : "status");
    // out[0..2] are the long-standing three; out[3..15] are verb-specific extras
    // (rule 14: a result that matters must not depend on the shared log buffer).
    // 16 is the hard ABI limit — io_scalar_inband64_t is uint64_t[16].
    if (in == 99) return cmd_scdcread(arg, arg2, arg3);   // 0.0.633: own layout (navi48_dispread.h)
    if (in == 91) return cmd_ddcread(arg, arg2);          // 0.0.622: own paging (two calls per EDID block), own layout (navi48_dispread.h)
    if (in == 92) return cmd_dmubring(arg);
    if (in == 93) return cmd_dispcensus(arg);
    if (in == 94) return !strcmp(what, "region4dump") ? cmd_region4dump() : cmd_region4read(arg, arg2);   // 0.0.624: own paging (22 dwords a call), own layout (navi48_dispread.h)
    if (in == 95) return cmd_dmubsend(arg, arg2);          // 0.0.625: own layout (navi48_dmubcmd.h)
    if (in == 96) return cmd_dmubmode(arg);
    if (in == 97) return cmd_dmubctx(arg, arg2, arg3);
    if (in == 105) return cmd_fbpublish(arg);               // 0.0.652 (M5): own layout (Navi48DisplayOps.h)
    if (in == 108) return cmd_vramstat(arg);                // 0.0.663: own layout (Navi48Bringup.cpp accelExperiment, action 108)
    if (in == 107) return cmd_m6xstat(arg, arg2);           // 0.0.661 (M6 Stage 1b): own layout (dcn/navi48_dcn.cpp scanXReport); 0.0.662: arg2 = the page after an instance
    if (in == 106) return cmd_m6stat(arg);                  // 0.0.659 (M6 Stage 1a): own layout (native_disp.cpp n48disp_verb)
    if (in == 98) return cmd_disp2(arg, arg2);             // 0.0.631: own layout (navi48_disp2.h); 0.0.633: arg2 = the instance (1 monitor A, 2 monitor B)
    uint64_t out[16] = {0};
    uint32_t outCnt = 16;
    // 0.0.194: scalarInput[1] is a verb argument — `vmib`'s VA. Always sent, so
    // a kext that ignores it is unaffected; 0 means "use the verb's default".
    uint64_t in2[2] = { in, 0 };
    if (arg) in2[1] = strtoull(arg, NULL, 0);
    // 0.0.618 review: `pipevbl` with no argument would send 0 and turn the vblank timestamps OFF; require 0 or 1.
    if (in == 89 && !arg) { fprintf(stderr, "accel pipevbl: give 0 or 1 explicitly (read the state with `accel pipestat 4`)\n"); return 2; }
    // 0.0.623 (G1): hangtest's only mode is 1 (the default); hangrecover's method is never defaulted (0 would run `auto`).
    if (in == 100 && !arg) in2[1] = 1;
    if (in == 103 && !arg) in2[1] = 4;   // 0.0.627 (G2): sessstat defaults to the global page
    if (in == 101 && !arg) { fprintf(stderr, "accel hangrecover: give the method: 0 auto, 1 release, 2 mesreset, 3 remap, 4 abandon (read the state with `accel hangstat`)\n"); return 2; }
    // 0.0.416 (an internal design note, G2): mode 7 carries its source VRAM offset as a SECOND CLI token
    // (`accel scanout 7 <vramOff> [gcr]`) and packs it into the ONE ABI scalar with the mode and the GCR flag
    // (sdma_gcr.h). All other verbs ignore arg2/arg3.
    // build 0.0.542: `accel scanout full [file]` is mode 9 (the optional file is written by scanfull_pull below).
    if (in == 60 && arg && !strcmp(arg, "full")) in2[1] = N48_SF_MODE;
    if (in == 60 && arg && strtoull(arg, NULL, 0) == 7) {
        const uint64_t off = arg2 ? strtoull(arg2, NULL, 0) : 0;
        const int gcr = (arg3 && !strcmp(arg3, "gcr")) ? 1 : 0;
        in2[1] = n48_scanout7_scalar(off, gcr);
    }
    if (in == 104) {   // 0.0.640 (G4): `appallow add <name>|remove <name>|list [page]` -> (op << 60) | key / page (Navi48AppKey.h); 0.0.641: `strikes`
        const int isStrikes = arg && !strcmp(arg, "strikes");
        const int isList = arg && !strcmp(arg, "list");
        const int isAdd = arg && !strcmp(arg, "add"), isRem = arg && !strcmp(arg, "remove");
        if ((!isList && !isAdd && !isRem && !isStrikes) || ((isAdd || isRem) && (!arg2 || !arg2[0]))) {
            fprintf(stderr, "accel appallow: usage: appallow add <name> | remove <name> | list [page 0..3] | strikes   (name = the executable's base name; only its first %u characters count)\n", N48A_COMM_MAX);
            return 2;
        }
        in2[1] = isStrikes ? n48a_arg(N48A_OP_STRIKES, 0) : isList ? n48a_arg(N48A_OP_LIST, arg2 ? strtoull(arg2, NULL, 0) : 0) : n48a_arg(isAdd ? N48A_OP_ADD : N48A_OP_REMOVE, n48a_comm_key(arg2));
        if (!isList && !isStrikes) printf("accel appallow: name \"%.16s\" -> key %#llx\n", arg2, (unsigned long long)n48a_comm_key(arg2));
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
    if (in == 104) {
        // 0.0.640 (G4): out[3 + i] = v[i] of amdgpu::n1c_app_verb (amd/native_g4_pure.h verb_run): v[0] code, v[1] entries after, v[2] op, v[3] max (32), v[4..12] key / the page's entries.
        static const char *cn[] = { "ok", "OFF: boot-arg navi48-apps or navi48-multisession is not 1", "bad argument", "FULL (32 entries)", "already on the list", "not on the list" };
        const unsigned long long c = out[3];
        printf("  code                    : %llu (%s)\n", c, c <= 5 ? cn[c] : "?");
        printf("  entries on the list     : %llu of %llu\n", out[4], out[6]);
        if (c == 0 && out[5] == N48A_OP_STRIKES) {   // 0.0.641: out[7 + 2i] = key, out[8 + 2i] = strikes of table slot i, out[15] = the limit
            printf("  strikes (automatic recoveries blamed on an APP session, per executable-name key; at %llu the name is refused at open until reboot)\n", out[15]);
            for (unsigned i = 0; i < 4; i++) printf("    slot %u                : key %#llx  strikes %llu%s\n", i, out[7 + 2 * i], out[8 + 2 * i], out[7 + 2 * i] == 0 ? "  (empty)" : out[8 + 2 * i] >= out[15] ? "  REFUSED" : "");
        }
        if (c == 0 && out[5] == N48A_OP_LIST) {
            printf("  page                    : keys of slots %llu..%llu (0 = empty slot)\n", (arg2 ? strtoull(arg2, NULL, 0) : 0) * 9, (arg2 ? strtoull(arg2, NULL, 0) : 0) * 9 + 8);
            for (unsigned i = 0; i < 9; i++) printf("    slot %2llu              : %#llx\n", (arg2 ? strtoull(arg2, NULL, 0) : 0) * 9 + i, out[7 + i]);
        }
        return c == 0 || c == 4 || c == 5 ? 0 : 3;
    }
    if (in == 103) {
        // 0.0.627 (G2): out[3 + i] = v[i] of amdgpu::n1c_g2_stat (amd/native_g2_pure.h sess_out / global_out); v[0] is the code.
        const unsigned long long c = out[3];
        printf("  code                    : %llu (%s)\n", c, c == 0 ? "ok" : c == 1 ? "OFF: boot-arg navi48-multisession is not 1" : c == 2 ? "bad page" : "?");
        if (c == 0 && out[4] == 4) {
            printf("  open / DEAD slots / HUNG: %llu / %llu / %llu\n", out[5] & 0xff, (out[5] >> 8) & 0xff, (out[5] >> 16) & 1);
            if ((out[5] >> 20) & 0xffff) printf("  BOs leaked at close     : %llu (a CPU mapping outlived the session: the range stays out of the pool until reboot; 0.0.640)\n", (out[5] >> 20) & 0xffff);
            if ((out[5] >> 56) != 0)   /* 0.0.641: apps ON: the visible-pool use of non-WindowServer sessions and its cap (boot-arg navi48-appvis) */
                printf("  non-WS visible VRAM     : %llu KiB held of a %llu MiB cap (boot-arg navi48-appvis, default 64; WindowServer is not capped)\n", (out[5] >> 36) & 0xfffff, (out[5] >> 56));
            printf("  seq emitted / retired   : %llu / %llu\n", out[6], out[7]);
            printf("  other sessions' VRAM    : %llu MiB of a %llu MiB budget\n", out[8] >> 20, out[9] >> 20);
            printf("  automatic recoveries    : %llu (last guilty slot %llu, seq %llu; collateral submissions %llu; at most 3 per boot)\n", out[10], out[11], out[12], out[13]);
            /* 0.0.629 (R5): the GCVM L2 protection-fault word (STATUS_LO32 | ADDR_LO32 << 32), READ ONLY: now, and at the previous `sessstat 4` */
            printf("  gcvm fault now / prev   : %#llx (vmid %llu) / %#llx (vmid %llu)%s\n", out[14], (out[14] >> 20) & 0xf, out[15], (out[15] >> 20) & 0xf,
                   out[14] != out[15] ? "  CHANGED since the previous sessstat 4" : "");
        } else if (c == 0 && out[4] == 5) {
            /* 0.0.650 (G5 Stage 1): out[3 + i] = v[i] of amdgpu::n1c_g5_stat (amd/native_g5_pure.h stat_out); exists only with boot-arg navi48-g5=1 */
            printf("  G5 flags                : G5 on%s%s\n", (out[5] & 2) ? ", WindowServer ACTIVE (submitted in the last 100 ms)" : ", WindowServer idle", (out[5] & 4) ? ", WindowServer open" : ", no WindowServer open");
            printf("  credit K                : %llu app job(s) may be in flight ahead of WindowServer (boot-arg navi48-g5credit, default 1)\n", out[6]);
            printf("  app jobs in flight now  : %llu    WindowServer seqnos outstanding now: %llu\n", out[7], out[8]);
            printf("  app submits delayed     : %llu    total delay %llu ms    maximum delay %llu ms\n", out[9], out[10], out[11]);
            printf("  credit-wait timeouts    : %llu    app submits admitted: %llu\n", out[12], out[13]);
            if (out[14] == ~0ull) printf("  since WindowServer's last submit : never\n"); else printf("  since WindowServer's last submit : %llu ms\n", out[14]);
            printf("  WindowServer ring reserve: %llu dwords (only WindowServer may use the last of the ring)\n", out[15]);
        } else if (c == 0) {
            const unsigned long long st = out[5] & 0xff;
            printf("  slot / VMID / session   : %llu / %llu / %llu\n", out[4], out[6], out[7]);
            printf("  state                   : %s%s%s%s\n", st == 0 ? "free" : st == 1 ? "OPEN" : st == 2 ? "DEAD (its VMID is never reused this boot)" : "?",
                   (out[5] & 0x100) ? ", WindowServer" : "", (out[5] & 0x200) ? ", hello" : "", (out[5] & 0x400) ? ", POISONED (guilty of a recovered hang)" : "");
            if (out[5] & 0x800) printf("  role                    : APP (an allow-listed user application: no ReadRegs, no scanout, the 25%% VRAM budget; 0.0.640)\n");
            printf("  VRAM / GTT / imported   : %llu / %llu / %llu KiB\n", out[8] >> 10, out[9] >> 10, out[10] >> 10);
            printf("  page tables / last seq / BOs : %llu KiB / %llu / %llu\n", out[11] >> 10, out[12], out[13]);
            printf("  gcvm fault at open      : %#llx (vmid %llu)   (0.0.629: compare with sessstat 4)\n", out[14], (out[14] >> 20) & 0xf);   /* R5, read only */
        }
        return c == 0 ? 0 : 3;
    }
    if (in == 100 || in == 101 || in == 102) {
        // 0.0.623 (G1): out[3 + i] = v[i] of amdgpu::n1c_g1_verb; v[0] is the code (amd/native_g1_pure.h enum Code).
        const unsigned long long c = out[3];
        const char *cn = c == 0 ? "ok" : c == 1 ? "REFUSED: boot-arg navi48-g1 is not 1" : c == 2 ? "REFUSED: bad mode / method" : c == 3 ? (in == 100 ? "REFUSED: a test context already holds the GPU" : "REFUSED: no test context holds a hang")
                       : c == 4 ? (in == 100 ? "REFUSED: a WindowServer native session is open" : "REFUSED: HUNG is not latched") : c == 5 ? (in == 100 ? "REFUSED: a native session is open" : "REFUSED: attempt budget spent")
                       : c == 6 ? (in == 100 ? "REFUSED: the GPU is HUNG" : "REFUSED: a MES frame timed out earlier in this boot (only release and abandon remain; reboot)")
                       : c == 7 ? "REFUSED: remap needs the most recent MES RESET on this hang to have been acknowledged (run mesreset first)" : c == 10 ? "HANG HELD: the IB did not retire in 2 s, HUNG latched, the test context holds the GPU"
                       : c == 11 ? "no hang: the IB retired (closed normally)" : c == 12 ? "the native open was refused (see rc)" : c == 13 ? "test BO setup failed" : c == 14 ? "the ring refused the submit"
                       : c == 20 ? "RECOVERED: a probe retired and read back, HUNG cleared, test context closed" : c == 21 ? "ALL FAILED: HUNG stays latched" : c == 22 ? "abandoned: HUNG stays latched (reboot)"
                       : c == 30 ? "verified" : c == 31 ? "the method's own step was not acked" : c == 32 ? "probe refused by the ring" : c == 33 ? "probe did not retire in 1 s" : c == 34 ? "probe retired, data wrong" : "?";
        printf("  code                    : %llu (%s)\n", c, cn);
        if (in == 100) {
            printf("  rc / seq                : %#llx / %llu\n", out[4], out[5]);
            printf("  emitted / retired       : %llu / %llu\n", out[6], out[7]);
            printf("  CP_STAT / rptr / wc     : %#llx / %llu / %llu\n", out[8], out[9], out[10]);
            printf("  GCVM fault status lo    : %#llx\n", out[11]);
        } else if (in == 101) {
            printf("  attempts this call      : %llu\n", out[4]);
            for (unsigned i = 0; i < 2 && i < out[4]; i++)
                printf("  attempt %u               : status %llu, act rc %#llx, verify %llu us\n", i, out[5 + 3 * i], out[6 + 3 * i], out[7 + 3 * i]);
            printf("  HUNG cleared / still    : %llu / %llu\n", out[11], out[12]);
            printf("  emitted / retired       : %llu / %llu (attempts on this hang %llu)\n", out[13], out[14], out[15]);
        } else if (arg == NULL || strtoull(arg, NULL, 0) == 0) {
            printf("  phase / HUNG / holds    : %llu / %llu / %llu\n", out[4], out[5], out[6]);
            printf("  hung seq / emitted / retired / wc : %llu / %llu / %llu / %llu\n", out[7], out[8], out[9], out[10]);
            printf("  attempts / last method  : %llu / %llu\n", out[11], out[12]);
            printf("  tests / recovered / failed runs : %llu / %llu / %llu\n", out[13], out[14], out[15]);
        } else {
            printf("  method / act rc / act us: %llu / %#llx / %llu\n", out[4], out[5], out[6]);
            printf("  probe rc / seq          : %#llx / %llu\n", out[7], out[8]);
            printf("  retired / data ok / us  : %llu / %llu / %llu\n", out[9], out[10], out[11]);
            printf("  rptr / CP_STAT after    : %llu / %#llx\n", out[12], out[13]);
        }
        if (in == 101 && c != 20 && c != 22) return 3;
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
                                     /* 0.0.416 mode 7 (an internal design note G2) */
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
            /* 0.0.414 (an internal design note, section 950): the FULL-GEOMETRY SDMA self-test.
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
            /* 0.0.416 (an internal design note, section 953): the SDMA CACHE-RINSE INSTRUMENT. READ-ONLY on the
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
            /* D7 (0.0.417, an internal design note): THE UNIFORM PROBE. Mode 6's live 1920x1080 case
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
        /* D1 (0.0.417, an internal design note): out[i] = v[i] of navi48_sdmadcc_control.
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
        fprintf(stderr, "usage: %s info|counters|reg <dw>|regs (list on stdin)|poke|submit|log|metrics|power <n>|logreset|\n       accel [status|fire [1 gate closed|2 gate open]|memenable|synctables|enablerings|startengines|ringstate|dumpring|programqueue|ringhooks|dumpib|rebaseib|enablequeue|kickdoorbell|ringrefs|queuestate|opengate|neuterpoll|chanstate|pokecompletion|signalcompletion|bindchannel|schedstate|stampstate|signalstamp|stampgap|runcheckts|runadvance|xlatregs|srbmprobe|resume|pm4powerup|setvspace|kiqenable|kiqstamp|kiqchan|gfxmap|gfxstate|sdmamap|sdmastate|faultclear|vmstate|vmib [va]|ringib [chan]|pagecopy [1]|flushdrop|kernsub [1|2|3]|vmpage [va]|renderxlat [0|1|3|4|5|0x101|base|flags|n<<4|6 or 7]|eopbridge [1|2]|bootchain [mode]|shadercache [1|2|3]|vmroots [addr]|ringmap [0|1]|vmctx|rootwrite [0|1]|rearmdrain [0|1]|pairing [1|2]|drain|flushhook [1|2|3]|scanout [0|1|2|3|4|5|6|7 <vramOff> [gcr]|8|full [file]|10]|sdmadcc [0|1|2]|gfxcensus [1|2]|gfxneuter [1|2]|finishread|gfxcapture [1|2]|gfxprobe [1|2]|pipeguard [1]|agdc [1]|agdchold [ms 1..5000]|cqprobe|ucprobe [0|1|2]|fbbench [rows]|fbwc [1]|pipeshim [0|1|2|3|4|5]|pipemode [0|1]|emcensus [0|1|2]|routea [0|1|2]|dcnstate [1]|dcnvbl [0|1|2]|dcnflip [0|1|2..30|1002..1240]|dcnmode [0|1..30|101..130]|fbname [0|1]|pipeadopt|pipearm [0|1]|pipestat [0|1|2|3|4]|pipestamps|pipeshortcut [0|1]|pipeagdc [0|1]|pipevbl [0|1]|pipereload [0|1]|ddcread <line 2|3> <block 0..3>|dmubring [0|1..25|0x80]|dispcensus [0|1|2..9]|region4read <off> [1..64]|region4dump|dmubsend detect <ctx>/begin/end/setmode <ctx>/enable <ctx>/disable <0x7000|0x7800>/replay <slot_off>/pclk otg3-on|otg3-off|otg1-on|otg1-off/phyc enable|disable/digc setup|dmubmode save/restore/0x1a6/0x1d4|dmubctx <ctx> status <v>|disp2 [timing|timing1440|connect|off|status [1|2]|plane|show|flipA|flipB|crc|planeoff 2]|hangtest [1]|hangrecover <0..4>|hangstat [0..8]|sessstat [0..5]|scdcread <line 2|3> <off 0..0x5f> [len 1..16]|appallow add <name>/remove <name>/list/strikes]|fbpublish 2|1|m6stat [0|1|2|3]|m6xstat [0|1|2]|vramstat|capstream <file> [ms] [s] [stopfile]|bigmem [mb]|test <id> <iters> [us]|suite [iters]\n", argv[0]);
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
