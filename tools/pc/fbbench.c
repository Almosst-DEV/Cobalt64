// fbbench — Part A of the display brief: how fast can a process write into RDNA4FB's framebuffer
// mapping, per cache mode, against the same copy into system RAM.
//
// WHY: CoreDisplay's legacy present  copies every dirty rectangle from its CPU shadow into the
// kIOFBVRAMMemory (0x6e) mapping of the framebuffer. CGXMappedDisplayStart picks that mapping's cache option from
// IOPixelInformation.flags (CoreDisplay 0x7ff80540879d-0x7ff8054087e3): kIOMapAnywhere | kIOMapCopybackCache (0x301)
// when flags has 0x10000, | kIOMapWriteThruCache (0x201) when flags has 0x20000, otherwise kIOMapAnywhere alone (0x1),
// i.e. the default cache mode. RDNA4FB::getPixelInformation leaves flags 0 (src/RDNA4FB/src/framebuffer.cpp), so
// WindowServer maps the BAR with the default mode, which for a device page the kernel treats like kIOMapInhibitCache
// (IOMapPages 0xffffff8000482a30: default -> WIMG 7, the same value as 0x100). This tool measures what that costs.
//
// WHAT IT DOES (nothing else):
//   - target=rdna4fb: opens RDNA4FB's SHARED connection (type 1, kIOFBSharedConnectType), never type 0 (WindowServer's);
//     target=navi48sc|navi48vr (0.0.287, the default navi48sc): our Navi48Bringup user client's scanout memory types;
//   - for each cache option asked for (default 0x1 0x101 0x201 0x301 0x401), IOConnectMapMemory64(conn, 0x6e, ...),
//     reads the mapped framebuffer into RAM once, then writes THE SAME BYTES back (so the screen does not change
//     unless WindowServer redraws in between), timing: a whole-mapping memcpy (x reps), a 256x256-pixel rectangle
//     loop and a 64x64 loop; then unmaps;
//   - times the same memcpy sizes RAM -> RAM as the baseline.
// No shape, no lock, no flush, no accelerator, no Metal. Run as root on an UNARMED boot at the login window.
//
// Build on the host Mac (tools/stage-to-pc.sh does):
//   clang -arch x86_64 -mmacosx-version-min=12.0 -O2 -Wall -framework IOKit -framework CoreFoundation \
//         tools/pc/fbbench.c -o tools/pc/fbbench
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <IOKit/IOKitLib.h>
#include <CoreFoundation/CoreFoundation.h>

#define FBBENCH_TOKEN "fbbench: cache-mode sweep v3"

static double now_s(void) {
    static mach_timebase_info_data_t tb;
    if (!tb.denom) mach_timebase_info(&tb);
    return (double)mach_absolute_time() * tb.numer / tb.denom / 1e9;
}

static void ram_baseline(size_t n) {
    uint8_t *a = malloc(n), *b = malloc(n);
    if (!a || !b) { printf("ram: malloc failed\n"); free(a); free(b); return; }
    for (size_t i = 0; i < n; i++) a[i] = (uint8_t)(i * 2654435761u >> 13);
    memcpy(b, a, n);                                    // fault both in
    double best = 1e9;
    volatile uint64_t sum = 0;                          // read b back so the copy cannot be elided
    for (int r = 0; r < 5; r++) {
        memset(b, r, n);
        double t = now_s(); memcpy(b, a, n); t = now_s() - t; if (t < best) best = t;
        for (size_t i = 0; i < n; i += 4096) sum += b[i];
    }
    printf("ram->ram  whole %zu bytes: best %.3f ms = %.1f MiB/s (checksum %llu)\n", n, best * 1e3, n / best / 1048576.0,
           (unsigned long long)sum);
    free(a); free(b);
}

static unsigned gRows = 128;

static void bench_mode(io_connect_t conn, uint32_t mtype, IOOptionBits opt, uint32_t stride, int reps) {
    mach_vm_address_t addr = 0; mach_vm_size_t size = 0;
    kern_return_t kr = IOConnectMapMemory64(conn, mtype, mach_task_self(), &addr, &size, opt);
    printf("\n== type %#x options 0x%x: IOConnectMapMemory64 kr=0x%x addr=0x%llx size=0x%llx\n", mtype, opt, kr,
           (unsigned long long)addr, (unsigned long long)size);
    if (kr != KERN_SUCCESS || !addr || !size) return;
    // 0.0.287: BAR reads measured ~0.57 MiB/s in agdc1, so the "whole" copy is bounded to `rows` rows (default 128) - the read
    // of an 8 MiB mapping alone would take ~15 s per mode.
    size_t n = (size_t)size;
    if (gRows && (size_t)gRows * stride < n) n = (size_t)gRows * stride;
    uint8_t *snap = malloc(n);
    if (!snap) { printf("  malloc failed\n"); goto out; }
    double t = now_s(); memcpy(snap, (void *)addr, n); t = now_s() - t;
    printf("  read  fb->ram whole %zu bytes: %.3f ms = %.1f MiB/s\n", n, t * 1e3, n / t / 1048576.0);
    size_t nz = 0; for (size_t i = 0; i < n; i += 4096) nz += snap[i] != 0;
    printf("  snapshot sampled non-zero bytes (every 4 KiB): %zu of %zu\n", nz, (n + 4095) / 4096);
    double best = 1e9, worst = 0;
    for (int r = 0; r < reps; r++) {
        t = now_s(); memcpy((void *)addr, snap, n); t = now_s() - t;
        if (t < best) best = t; if (t > worst) worst = t;
        printf("  write ram->fb whole rep %d: %.3f ms\n", r, t * 1e3);
        if (t > 5.0) { printf("  (a single copy took over 5 s - stopping the reps)\n"); break; }
    }
    printf("  WRITE WHOLE options 0x%x: best %.3f ms worst %.3f ms = %.1f MiB/s\n", opt, best * 1e3, worst * 1e3,
           n / best / 1048576.0);
    // Rectangle loops at the top-left, the same bytes written back.
    if (stride >= 4) {
        const uint32_t w[2] = { 128, 64 };
        for (int k = 0; k < 2; k++) {
            size_t rowb = (size_t)w[k] * 4, rows = w[k], iters = (k == 0) ? 64 : 1024;
            if ((rows * stride) > n || rowb > stride) continue;
            t = now_s();
            for (size_t it = 0; it < iters; it++)
                for (size_t y = 0; y < rows; y++)
                    memcpy((uint8_t *)addr + y * stride, snap + y * stride, rowb);
            t = now_s() - t;
            size_t bytes = iters * rows * rowb;
            printf("  WRITE RECT %ux%u x%zu options 0x%x: %.3f ms total, %.3f ms per rect = %.1f MiB/s\n", w[k], w[k],
                   iters, opt, t * 1e3, t * 1e3 / iters, bytes / t / 1048576.0);
        }
    }
out:
    free(snap);
    kr = IOConnectUnmapMemory64(conn, mtype, mach_task_self(), addr);
    printf("  unmap kr=0x%x\n", kr);
}

int main(int argc, char **argv) {
    printf("%s\n", FBBENCH_TOKEN);
    int reps = 3;
    const char *target = "navi48sc";
    IOOptionBits opts[8] = { 0x1, 0x101, 0x201, 0x301, 0x401 }; int nopt = 5, userOpts = 0;
    for (int i = 1; i < argc; i++) {
        if (!strncmp(argv[i], "reps=", 5)) { reps = atoi(argv[i] + 5); continue; }
        if (!strncmp(argv[i], "rows=", 5)) { gRows = (unsigned)atoi(argv[i] + 5); continue; }
        if (!strncmp(argv[i], "target=", 7)) { target = argv[i] + 7; continue; }
        if (!userOpts) { nopt = 0; userOpts = 1; }
        if (nopt < 8) opts[nopt++] = (IOOptionBits)strtoul(argv[i], NULL, 0);
    }
    // target=rdna4fb: the framebuffer's shared user client, type 0x6e (refused at the login window, an earlier analysis).
    // target=navi48sc / navi48vr (0.0.287): our own user client, memory type 'FBSC' (a fresh device range over the scanout) or
    // 'FBVR' (RDNA4FB's own getVRAMRange descriptor, which an armed fbwc wraps); the options go through IOUserClient's
    // mapClientMemory64 to createMappingInTask exactly as CoreDisplay's do.
    int toRdna = !strcmp(target, "rdna4fb");
    const char *svcName = toRdna ? "RDNA4FB" : "Navi48Bringup";
    uint32_t mtype = toRdna ? 0x6e : (!strcmp(target, "navi48vr") ? 0x46425652u : 0x46425343u);
    // : the runtime class rename makes the leaf class AMDRDNA4FB while armed; try both.
    io_service_t fb = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("AMDRDNA4FB"));
    if (!fb) fb = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("RDNA4FB"));
    if (!fb) { printf("neither AMDRDNA4FB nor RDNA4FB found\n"); return 2; }
    uint32_t stride = 1920 * 4, height = 0;
    CFTypeRef v = IORegistryEntryCreateCFProperty(fb, CFSTR("Console,RowBytes"), kCFAllocatorDefault, 0);
    if (v && CFGetTypeID(v) == CFNumberGetTypeID()) CFNumberGetValue((CFNumberRef)v, kCFNumberSInt32Type, &stride);
    if (v) CFRelease(v);
    v = IORegistryEntryCreateCFProperty(fb, CFSTR("Console,Height"), kCFAllocatorDefault, 0);
    if (v && CFGetTypeID(v) == CFNumberGetTypeID()) CFNumberGetValue((CFNumberRef)v, kCFNumberSInt32Type, &height);
    if (v) CFRelease(v);
    printf("target %s (service %s, memory type %#x); stride %u, height %u; rows per copy %u\n", target, svcName, mtype, stride, height, gRows);
    io_service_t svc = toRdna ? fb : IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching(svcName));
    if (!svc) { printf("%s not found\n", svcName); IOObjectRelease(fb); return 2; }
    io_connect_t conn = IO_OBJECT_NULL;
    kern_return_t kr = IOServiceOpen(svc, mach_task_self(), toRdna ? 1 /* kIOFBSharedConnectType */ : 0, &conn);
    printf("IOServiceOpen(%s, type %d) kr=0x%x\n", svcName, toRdna ? 1 : 0, kr);
    if (kr != KERN_SUCCESS) { if (!toRdna) IOObjectRelease(svc); IOObjectRelease(fb); return 3; }
    ram_baseline((size_t)gRows * stride);
    for (int i = 0; i < nopt; i++) bench_mode(conn, mtype, opts[i], stride, reps);
    kr = IOServiceClose(conn);
    printf("\nIOServiceClose kr=0x%x\n", kr);
    if (!toRdna) IOObjectRelease(svc);
    IOObjectRelease(fb);
    return 0;
}
