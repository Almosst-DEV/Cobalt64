// m4-adopt.c — milestone 4, route c′ wall 1 (adoption) probe. AIR-BUILT, PC-RUN, READ-MOSTLY.
//
// Purpose (notes/M4-CPRIME-DESIGN.md): on an armed login-window boot, prove that
// IOServiceRequestProbe(accelerator, 1) enrols RDNA4FB into Apple's IOAccelDisplayMachine
// without harming the machine, and that IOAccelCreateSurface then starts a surface where
// test A's WindowServer failed with "failed to get the vram descriptors".
//
// This tool takes NO lock, sets NO shape, does NO flush, submits NO command buffer and
// makes NO Metal call. It creates and immediately destroys one accelerator surface, twice,
// and requests the accelerator's probe once (with a no-op control first).
//
// Six steps, every return code printed:
//   1  find the AMDRadeonX6000_AMDNavi21GraphicsAccelerator service and the RDNA4FB framebuffer
//   2  print RDNA4FB's IOAccelTypes / IOAccelIndex / IOAccelRevision / IOCFPlugInTypes
//   3  IOAccelCreateSurface + IOAccelDestroySurface  (BEFORE adoption: expected to fail; the
//      "failed to get the vram descriptors" kernel line is this run's positive control)
//   4  IOServiceRequestProbe(accel, 0)   -- negative control
//   5  IOServiceRequestProbe(accel, 1)   -- adoption
//   6  repeat 2 and 3                     (AFTER adoption: expected to succeed)
//
// IOAccelCreateSurface is replicated inline from IOKit _IOAccelCreateSurface @ 0x7ff806c8c7d0
// (re/m3b/IOKit) so the tool links against public IOKitLib only:
//   type   = (flags >> 9) & 0x20                          (0x7ff806c8c800-806)
//   open   = IOServiceOpen(accel, mach_task_self(), type, &conn)   (0x7ff806c8c816)
//   create = IOConnectCallScalarMethod(conn, 7, {id, flags}, 2, NULL, NULL)  (0x7ff806c8c836-846)
//   ok     -> the surface persists on that connection; destroy = IOServiceClose(conn)
// The 32-bpp surface flags value 0x4233b666 is what CoreDisplay's setup_hardware passes for a
// 32-bpp display device (CoreDisplay 0x7ff8053fbc3e-4f: (bpp==32)<<17 | 0x4231b666, and
// 0x4231b666 | 0x20000 = 0x4233b666).

#include <IOKit/IOKitLib.h>
#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach.h>
#include <stdio.h>
#include <stdint.h>

static const char *kAccelClass = "AMDRadeonX6000_AMDNavi21GraphicsAccelerator";
// : while the runtime class rename is armed the leaf class is AMDRDNA4FB, so a lookup
// by the old name alone finds nothing. Try the new name first, then the old.
static const char *kFbClass    = "RDNA4FB";
static const char *kFbClassAlt = "AMDRDNA4FB";

// 32-bpp display-device surface flags, per CoreDisplay setup_hardware (see header).
static const uint32_t kSurfFlags = 0x4233b666u;
static const uint32_t kSurfId    = 1u;

static io_service_t find_one(const char *cls) {
    // matchingServices consumes one reference on the dictionary.
    CFMutableDictionaryRef m = IOServiceMatching(cls);
    if (!m) return IO_OBJECT_NULL;
    io_iterator_t it = IO_OBJECT_NULL;
    kern_return_t kr = IOServiceGetMatchingServices(kIOMainPortDefault, m, &it);
    if (kr != KERN_SUCCESS || it == IO_OBJECT_NULL) {
        fprintf(stderr, "  IOServiceGetMatchingServices(%s) kr=0x%x\n", cls, kr);
        return IO_OBJECT_NULL;
    }
    io_service_t svc = IOIteratorNext(it);            // first match; caller releases
    io_service_t extra = IOIteratorNext(it);
    if (extra) { printf("  NOTE: more than one %s matched; using the first\n", cls); IOObjectRelease(extra); }
    IOObjectRelease(it);
    return svc;
}

static void print_reg_id(const char *label, io_service_t s) {
    uint64_t id = 0;
    kern_return_t kr = IORegistryEntryGetRegistryEntryID(s, &id);
    printf("  %s regID=0x%llx (kr=0x%x)\n", label, (unsigned long long)id, kr);
}

static void print_str_prop(io_service_t s, const char *key) {
    CFStringRef k = CFStringCreateWithCString(kCFAllocatorDefault, key, kCFStringEncodingUTF8);
    CFTypeRef v = IORegistryEntryCreateCFProperty(s, k, kCFAllocatorDefault, 0);
    CFRelease(k);
    if (!v) { printf("  %-16s : <absent>\n", key); return; }
    if (CFGetTypeID(v) == CFStringGetTypeID()) {
        char buf[1024];
        if (CFStringGetCString((CFStringRef)v, buf, sizeof buf, kCFStringEncodingUTF8))
            printf("  %-16s : \"%s\"\n", key, buf);
        else printf("  %-16s : <string, unprintable>\n", key);
    } else if (CFGetTypeID(v) == CFNumberGetTypeID()) {
        long long n = 0; CFNumberGetValue((CFNumberRef)v, kCFNumberLongLongType, &n);
        printf("  %-16s : %lld\n", key, n);
    } else if (CFGetTypeID(v) == CFDictionaryGetTypeID()) {
        printf("  %-16s : <dictionary, present>\n", key);
    } else {
        printf("  %-16s : <present, type %ld>\n", key, (long)CFGetTypeID(v));
    }
    CFRelease(v);
}

static void dump_fb_keys(io_service_t fb, const char *when) {
    printf("[keys %s]\n", when);
    print_str_prop(fb, "IOAccelTypes");
    print_str_prop(fb, "IOAccelIndex");
    print_str_prop(fb, "IOAccelRevision");
    print_str_prop(fb, "IOCFPlugInTypes");
}

// Inline replica of IOAccelCreateSurface (IOKit 0x7ff806c8c7d0). Returns the create kr;
// on success *outConn holds the surface connection to close with IOServiceClose.
static kern_return_t accel_create_surface(io_service_t accel, uint32_t id, uint32_t flags,
                                          io_connect_t *outConn) {
    *outConn = IO_OBJECT_NULL;
    uint32_t type = (flags >> 9) & 0x20u;                 // 0x7ff806c8c800
    io_connect_t conn = IO_OBJECT_NULL;
    kern_return_t kr = IOServiceOpen(accel, mach_task_self(), type, &conn);   // 0x7ff806c8c816
    printf("  IOServiceOpen(accel, type=0x%x) kr=0x%x\n", type, kr);
    if (kr != KERN_SUCCESS) return kr;
    uint64_t in[2] = { id, flags };
    kr = IOConnectCallScalarMethod(conn, 7, in, 2, NULL, NULL);               // 0x7ff806c8c846
    printf("  IOConnectCallScalarMethod(conn, sel=7, {0x%x, 0x%x}) kr=0x%x\n", id, flags, kr);
    if (kr == KERN_SUCCESS) { *outConn = conn; return kr; }
    IOServiceClose(conn);
    return kr;
}

static void surface_probe(io_service_t accel, const char *when) {
    printf("[surface %s]\n", when);
    io_connect_t conn = IO_OBJECT_NULL;
    kern_return_t kr = accel_create_surface(accel, kSurfId, kSurfFlags, &conn);
    if (kr == KERN_SUCCESS && conn) {
        kern_return_t dr = IOServiceClose(conn);          // IOAccelDestroySurface
        printf("  IOAccelDestroySurface kr=0x%x\n", dr);
    }
    printf("  surface-create verdict: %s (kr=0x%x)\n",
           kr == KERN_SUCCESS ? "SUCCESS" : "FAIL", kr);
}

int main(void) {
    printf("=== m4-adopt: route c' wall 1 (adoption) probe ===\n");

    // 1. find services
    io_service_t accel = find_one(kAccelClass);
    io_service_t fb    = find_one(kFbClassAlt);
    if (!fb) fb = find_one(kFbClass);
    printf("[services]\n");
    if (!accel) { printf("  accelerator %s: NOT FOUND (is the accelerator armed?)\n", kAccelClass);
                  if (fb) IOObjectRelease(fb); return 2; }
    if (!fb)    { printf("  framebuffer %s or %s: NOT FOUND\n", kFbClassAlt, kFbClass);
                  IOObjectRelease(accel); return 2; }
    print_reg_id("accelerator", accel);
    print_reg_id("framebuffer", fb);

    // 2. keys before adoption
    dump_fb_keys(fb, "before");

    // 3. surface before adoption (expected FAIL; positive control is the kernel descriptor line)
    surface_probe(accel, "before");

    // 4. negative control
    kern_return_t kr0 = IOServiceRequestProbe(accel, 0);
    printf("[probe] IOServiceRequestProbe(accel, 0) kr=0x%x\n", kr0);

    // 5. adoption
    kern_return_t kr1 = IOServiceRequestProbe(accel, 1);
    printf("[probe] IOServiceRequestProbe(accel, 1) kr=0x%x\n", kr1);

    // 6. keys and surface after adoption (expected SUCCESS)
    dump_fb_keys(fb, "after");
    surface_probe(accel, "after");

    IOObjectRelease(accel);
    IOObjectRelease(fb);
    printf("=== done ===\n");
    return 0;
}
