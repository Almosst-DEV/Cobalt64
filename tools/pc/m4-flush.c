// m4-flush.c — milestone 4 route c' walls 2-3 test client (0.0.272). AIR-BUILT, PC-RUN.
//
// A sibling of m4-adopt.c. On an ADOPTED boot (m4-adopt ran first) it plays the part CoreDisplay's mapped-display
// path plays, with ONE deliberate difference, and prints every return code:
//   1  find AMDRadeonX6000_AMDNavi21GraphicsAccelerator and RDNA4FB; print RDNA4FB's IOAccelTypes (adopted or not)
//   2  IOAccelCreateSurface, replicated as m4-adopt does: IOServiceOpen(type (mode>>9)&0x20) + selector 7 {id, mode}.
//      THE DIFFERENCE: id 0x1000, not CoreDisplay's MPAcquireSurfaceID() value (1..256). IOAccelSurface::set_id_mode
//      @ IOAcceleratorFamily2 0x145b7816 refuses every id below 256 ("Assembly surfaces for display updates not
//      supported", 145b79ef: 41 81 fe ff 00 00 00), . Mode (0.0.272 flush2) 0x24 = 8888 +
//      windowed: flush1 used CoreDisplay's 0x8424, whose 0x400 front-buffer forcing bit makes the SHAPE a silent
//      BadArgument on this class (set_shape_backing_length_ext 145b8f51: a9 00 0c 00 00).
//   3  IOAccelSetSurfaceFramebufferShape: selector 9, scalars {shape options 0, framebuffer index 0}, region {1 rect,
//      bounds 0,0,W,H, rect 0,0,W,H} (0x14 bytes). NOT setup_hardware's {0x4000, 1x1}: option bit 14 is the other
//      silent BadArgument (145b8f4b: c1 ea 0e). --shape-options 0x4000 --width 1 --height 1 reproduces flush1.
//   4  IOAccelWriteLockSurfaceWithOptions: selector 3, scalar {options}, 0x58-byte IOAccelSurfaceInformation out
//      (setup_hardware's lock passes size 0x58, 7ff8053fbdd9). options 1 first, then 0 if refused
//   5  if the lock returned an address: write the test client's stripes (scanout_copy.h n48_client_pixel) into the
//      first --rows rows and read two rows back through the same mapping
//   6  IOAccelWriteUnlockSurfaceWithOptions: selector 4 {options}
//   7  IOAccelFlushSurfaceOnFramebuffers: selector 10, scalars {1 << 0, 0} (IOKit 7ff806c8cca4-7ff806c8ccc4)
//   8  hold --hold seconds (the kext reads the surface while it exists), then IOServiceClose
// No Metal, no command buffer, no submission. The kext's flush hook (`accel flushhook 1|3`) sees steps 2-7.
//
// Build (stage-to-pc.sh does this):
//   clang -arch x86_64 -mmacosx-version-min=12.0 -O2 -Wall -Wextra -I src/navi48-bringup/src/apple \
//         -framework IOKit -framework CoreFoundation tools/pc/m4-flush.c -o tools/pc/m4-flush

#include <IOKit/IOKitLib.h>
#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "scanout_copy.h"

static const char *kAccelClass = "AMDRadeonX6000_AMDNavi21GraphicsAccelerator";
// : while the runtime class rename is armed the leaf class is AMDRDNA4FB, so a lookup
// by the old name alone finds nothing. Try the new name first, then the old.
static const char *kFbClass    = "RDNA4FB";
static const char *kFbClassAlt = "AMDRDNA4FB";

static io_service_t find_one(const char *cls) {
    CFMutableDictionaryRef m = IOServiceMatching(cls);
    if (!m) return IO_OBJECT_NULL;
    io_iterator_t it = IO_OBJECT_NULL;
    kern_return_t kr = IOServiceGetMatchingServices(kIOMainPortDefault, m, &it);
    if (kr != KERN_SUCCESS || it == IO_OBJECT_NULL) {
        printf("  IOServiceGetMatchingServices(%s) kr=0x%x\n", cls, kr);
        return IO_OBJECT_NULL;
    }
    io_service_t svc = IOIteratorNext(it);
    io_service_t extra = IOIteratorNext(it);
    if (extra) { printf("  NOTE: more than one %s matched; using the first\n", cls); IOObjectRelease(extra); }
    IOObjectRelease(it);
    return svc;
}

static void print_types(io_service_t fb) {
    CFTypeRef v = IORegistryEntryCreateCFProperty(fb, CFSTR("IOAccelTypes"), kCFAllocatorDefault, 0);
    char buf[512] = "<absent>";
    if (v && CFGetTypeID(v) == CFStringGetTypeID()) CFStringGetCString((CFStringRef)v, buf, sizeof buf, kCFStringEncodingUTF8);
    printf("  RDNA4FB IOAccelTypes : %s%s\n", buf, v ? "" : "  (NOT ADOPTED: run m4-adopt first)");
    if (v) CFRelease(v);
}


static void dump_info(const uint8_t *b) {
    uint32_t d[22];
    memcpy(d, b, sizeof d);
    printf("  info: address 0x%016llx\n", (unsigned long long)((uint64_t)d[1] << 32 | d[0]));
    for (int i = 2; i < 22; i += 4)
        printf("  info +0x%02x: %08x %08x %08x %08x\n", i * 4, d[i], d[i + 1], i + 2 < 22 ? d[i + 2] : 0, i + 3 < 22 ? d[i + 3] : 0);
    printf("  info decoded: +0x20 %u  width(+0x24) %u  height(+0x28) %u  format(+0x2c) 0x%x\n", d[8], d[9], d[10], d[11]);
}

int main(int argc, char **argv) {
    uint32_t id = 0x1000, mode = 0x24, rows = 128, hold = 5, writeIt = 1, opts = 1;
    uint32_t shapeOpts = 0, width = 1920, height = 1080;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--id") && i + 1 < argc) id = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--mode") && i + 1 < argc) mode = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--rows") && i + 1 < argc) rows = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--hold") && i + 1 < argc) hold = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--lock-options") && i + 1 < argc) opts = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--shape-options") && i + 1 < argc) shapeOpts = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--width") && i + 1 < argc) width = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--height") && i + 1 < argc) height = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--no-write")) writeIt = 0;
        else { printf("usage: %s [--id N] [--mode M] [--rows R] [--hold S] [--lock-options O] [--shape-options S] [--width W] "
                      "[--height H] [--no-write]\n", argv[0]); return 2; }
    }
    if (rows > 1080) rows = 1080;
    if (width == 0 || width > 4096) width = 1920;
    if (height == 0 || height > 4096) height = 1080;
    if (hold > 120) hold = 120;
    printf("=== m4-flush: route c' walls 2-3 test client (id 0x%x, mode 0x%x, shape options 0x%x %ux%u, lock options %u, "
           "rows %u, hold %u s, write %u) ===\n", id, mode, shapeOpts, width, height, opts, rows, hold, writeIt);

    io_service_t accel = find_one(kAccelClass), fb = find_one(kFbClassAlt);
    if (!fb) fb = find_one(kFbClass);
    printf("[services]\n");
    if (!accel || !fb) { printf("  accelerator %s, framebuffer %s - ABORT\n", accel ? "found" : "NOT FOUND", fb ? "found" : "NOT FOUND");
                         if (accel) IOObjectRelease(accel); if (fb) IOObjectRelease(fb); return 2; }
    print_types(fb);

    // 2. create
    printf("[create]\n");
    io_connect_t conn = IO_OBJECT_NULL;
    uint32_t type = (mode >> 9) & 0x20u;
    kern_return_t kr = IOServiceOpen(accel, mach_task_self(), type, &conn);
    printf("  IOServiceOpen(accel, type=0x%x) kr=0x%x\n", type, kr);
    if (kr != KERN_SUCCESS) { IOObjectRelease(accel); IOObjectRelease(fb); return 3; }
    uint64_t cin[2] = { id, mode };
    kr = IOConnectCallScalarMethod(conn, 7, cin, 2, NULL, NULL);
    printf("  selector 7 {id 0x%x, mode 0x%x} kr=0x%x  create verdict: %s\n", id, mode, kr, kr ? "FAIL" : "SUCCESS");
    if (kr != KERN_SUCCESS) { IOServiceClose(conn); IOObjectRelease(accel); IOObjectRelease(fb); return 3; }

    // 3. shape: one rect, bounds and rect both 0,0,W,H (--shape-options 0x4000 --width 1 --height 1 is setup_hardware's form)
    printf("[shape]\n");
    uint8_t region[0x14] = { 0 };
    uint32_t one = 1; memcpy(region, &one, 4);
    int16_t bounds[4] = { 0, 0, (int16_t)width, (int16_t)height };
    memcpy(region + 4, bounds, 8);
    if (shapeOpts != 0x4000) memcpy(region + 12, bounds, 8);   // setup_hardware leaves rect[0] zero
    uint64_t sin[2] = { shapeOpts, 0 };
    kr = IOConnectCallMethod(conn, 9, sin, 2, region, sizeof region, NULL, NULL, NULL, NULL);
    printf("  selector 9 {0x%x, fb 0} region {1, 0,0,%u,%u, rect %s} kr=0x%x\n", shapeOpts, width, height,
           shapeOpts != 0x4000 ? "= bounds" : "0,0,0,0", kr);

    // 4. write lock
    printf("[lock]\n");
    uint8_t info[0x58];
    size_t isz = sizeof info;
    uint64_t lin[1] = { opts };
    memset(info, 0, sizeof info);
    kr = IOConnectCallMethod(conn, 3, lin, 1, NULL, 0, NULL, NULL, info, &isz);
    printf("  selector 3 WriteLockWithOptions {%u} kr=0x%x out size 0x%zx\n", opts, kr, isz);
    uint32_t lockedOpts = opts;
    if (kr != KERN_SUCCESS && opts != 0) {
        lin[0] = 0; isz = sizeof info; memset(info, 0, sizeof info);
        kr = IOConnectCallMethod(conn, 3, lin, 1, NULL, 0, NULL, NULL, info, &isz);
        lockedOpts = 0;
        printf("  selector 3 WriteLockWithOptions {0} kr=0x%x out size 0x%zx\n", kr, isz);
    }
    if (kr == KERN_SUCCESS) {
        dump_info(info);
        uint64_t addr = 0; memcpy(&addr, info, 8);
        uint32_t f20 = 0, w = 0, h = 0; memcpy(&f20, info + 0x20, 4); memcpy(&w, info + 0x24, 4); memcpy(&h, info + 0x28, 4);
        uint32_t stride = (f20 >= w * 4u && f20 <= w * 4u + 64u) ? f20 : w * 4u;
        printf("  stride used: %u (%s)\n", stride, stride == f20 ? "+0x20" : "width*4, +0x20 not plausible");
        if (writeIt && addr && w && h && w <= 8192 && h <= 8192) {
            uint32_t *px = (uint32_t *)(uintptr_t)addr;
            uint32_t nr = rows < h ? rows : h;
            for (uint32_t y = 0; y < nr; y++)
                for (uint32_t x = 0; x < w; x++) px[(uint64_t)y * (stride / 4u) + x] = n48_client_pixel(x, y);
            uint32_t bad = 0;
            for (uint32_t x = 0; x < w; x++) {
                if (px[x] != n48_client_pixel(x, 0)) bad++;
                if (nr > 1 && px[(uint64_t)(nr - 1) * (stride / 4u) + x] != n48_client_pixel(x, nr - 1)) bad++;
            }
            printf("  WROTE the client stripes into rows [0,%u) x %u px through the lock mapping; read back rows 0 and %u: "
                   "%u mismatch(es); samples (0,0) 0x%08x (4,0) 0x%08x (%u,16) 0x%08x\n", nr, w, nr - 1, bad, px[0], px[4],
                   w / 2, nr > 16 ? px[16u * (stride / 4u) + w / 2] : 0);
        } else {
            printf("  nothing written (write %u, address 0x%llx, %ux%u)\n", writeIt, (unsigned long long)addr, w, h);
        }
        // 6. unlock
        uint64_t uin[1] = { lockedOpts };
        kr = IOConnectCallMethod(conn, 4, uin, 1, NULL, 0, NULL, NULL, NULL, NULL);
        printf("[unlock] selector 4 WriteUnlockWithOptions {%u} kr=0x%x\n", lockedOpts, kr);
    }

    // 7. flush
    uint64_t fin[2] = { 1u << 0, 0 };
    kr = IOConnectCallMethod(conn, 10, fin, 2, NULL, 0, NULL, NULL, NULL, NULL);
    printf("[flush] selector 10 {mask 0x1, options 0} kr=0x%x\n", kr);

    // 8. hold, close
    printf("[hold] %u s\n", hold);
    fflush(stdout);
    if (hold) sleep(hold);
    kr = IOServiceClose(conn);
    printf("[close] IOServiceClose kr=0x%x\n=== done ===\n", kr);
    IOObjectRelease(accel);
    IOObjectRelease(fb);
    return 0;
}
