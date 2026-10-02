// display-registry — the display-pairing question (notes/DISPLAY-PAIRING.md Q5) read from the
// IORegistry ONLY. No Metal, no CoreGraphics/SkyLight, no WindowServer, no IOServiceOpen: nothing
// here can wait on the GPU, so it is the variant that may run with Apple's accelerator armed
// (an earlier analysis: once the accelerator is armed every Metal client blocks, and the Metal-based
// display-pairing probe hung in U state on run disp2). Still never with a user logged in.
//
// What it prints, per IOFramebuffer and per IOAccelerator: class, registry entry ID, the named
// pairing properties when present (read one by one with IORegistryEntryCreateCFProperty; no
// whole-dictionary dumps, so the accelerator's PerformanceStatistics is never serialised), and the
// IOService-plane path up to the owning IOPCIDevice with that device's registry ID and vendor/device
// IDs. Then, unless --no-find, IOKit's own IOAccelFindAccelerator(framebuffer) — the IOGraphicsLib
// registry lookup that pairs a framebuffer with an accelerator (exported by IOKit.framework, no SDK
// header, declared below). Build on the host Mac (tools/stage-to-pc.sh does):
//   clang -arch x86_64 -mmacosx-version-min=12.0 -O2 -Wall -framework IOKit -framework CoreFoundation \
//         tools/pc/display-registry.c -o tools/pc/display-registry
#include <stdio.h>
#include <string.h>
#include <IOKit/IOKitLib.h>
#include <CoreFoundation/CoreFoundation.h>

// IOKit.framework export (IOKit.tbd lists it; the header IOAccelSurfaceControl.h is not in the SDK).
extern kern_return_t IOAccelFindAccelerator(io_service_t framebuffer, io_service_t *pAccelerator,
                                            UInt32 *pFramebufferIndex);

static void print_cf(const char *key, CFTypeRef v) {
    char buf[512] = "?";
    if (!v) return;
    if (CFGetTypeID(v) == CFStringGetTypeID()) {
        CFStringGetCString((CFStringRef)v, buf, sizeof buf, kCFStringEncodingUTF8);
        printf("    %-24s = \"%s\"\n", key, buf);
    } else if (CFGetTypeID(v) == CFNumberGetTypeID()) {
        long long n = 0; CFNumberGetValue((CFNumberRef)v, kCFNumberLongLongType, &n);
        printf("    %-24s = %lld (0x%llx)\n", key, n, (unsigned long long)n);
    } else if (CFGetTypeID(v) == CFBooleanGetTypeID()) {
        printf("    %-24s = %s\n", key, CFBooleanGetValue((CFBooleanRef)v) ? "Yes" : "No");
    } else if (CFGetTypeID(v) == CFDataGetTypeID()) {
        CFIndex len = CFDataGetLength((CFDataRef)v); const UInt8 *p = CFDataGetBytePtr((CFDataRef)v);
        printf("    %-24s = <", key);
        for (CFIndex i = 0; i < len && i < 32; i++) printf("%02x", p[i]);
        printf("%s> (%ld bytes)\n", len > 32 ? "..." : "", (long)len);
    } else {
        CFStringRef d = CFCopyDescription(v);
        if (d) { CFStringGetCString(d, buf, sizeof buf, kCFStringEncodingUTF8); CFRelease(d); }
        buf[200] = 0;
        printf("    %-24s = %s\n", key, buf);
    }
}

static void print_props(io_registry_entry_t e, const char *const *keys) {
    for (int i = 0; keys[i]; i++) {
        CFStringRef k = CFStringCreateWithCString(NULL, keys[i], kCFStringEncodingUTF8);
        CFTypeRef v = IORegistryEntryCreateCFProperty(e, k, kCFAllocatorDefault, 0);
        if (v) { print_cf(keys[i], v); CFRelease(v); }
        CFRelease(k);
    }
}

static uint64_t reg_id(io_registry_entry_t e) {
    uint64_t id = 0; IORegistryEntryGetRegistryEntryID(e, &id); return id;
}

// Walk the IOService plane upward, printing each hop, until an IOPCIDevice (or the root).
static uint64_t print_path_to_pci(io_registry_entry_t e) {
    io_registry_entry_t cur = e; IOObjectRetain(cur);
    uint64_t pci = 0;
    for (int hop = 0; hop < 16; hop++) {
        io_registry_entry_t parent = IO_OBJECT_NULL;
        if (IORegistryEntryGetParentEntry(cur, kIOServicePlane, &parent) != KERN_SUCCESS) break;
        IOObjectRelease(cur); cur = parent;
        io_name_t cls = "", name = "";
        IOObjectGetClass(cur, cls); IORegistryEntryGetName(cur, name);
        printf("    parent[%d] %-28s name=%-20s regID=0x%llx\n", hop, cls, name,
               (unsigned long long)reg_id(cur));
        if (IOObjectConformsTo(cur, "IOPCIDevice")) {
            static const char *const pk[] = { "vendor-id", "device-id", "IOPCIExpressLinkStatus",
                                              "pcidebug", NULL };
            print_props(cur, pk);
            pci = reg_id(cur);
            break;
        }
    }
    IOObjectRelease(cur);
    return pci;
}

static const char *const kFbKeys[] = { "IOFBDependentID", "IOFBDependentIndex", "IOAccelIndex",
    "IOAccelTypes", "IOAccelRevision", "IOFramebufferOpenGLIndex", "IOFBGammaWidth",
    "IOFBCurrentPixelCount", "IOFBMemorySize", "AAPL,display-alias", "IOMatchedAtBoot", NULL };
static const char *const kAccelKeys[] = { "IOAccelIndex", "IOAccelTypes", "IOAccelRevision",
    "MetalPluginName", "MetalPluginClassName", "MetalStatisticsName", "IOGLBundleName",
    "IOGLESBundleName", "IOVARendererID", "GpuConfigurationSupported", "IOMatchedAtBoot", NULL };

static int list_class(const char *cls, const char *const *keys, io_service_t *out, int max) {
    io_iterator_t it = IO_OBJECT_NULL; int n = 0;
    if (IOServiceGetMatchingServices(kIOMainPortDefault, IOServiceMatching(cls), &it) != KERN_SUCCESS) {
        printf("%s: IOServiceGetMatchingServices failed\n", cls); return 0;
    }
    io_service_t s;
    while ((s = IOIteratorNext(it)) != IO_OBJECT_NULL) {
        io_name_t c = "", name = "";
        IOObjectGetClass(s, c); IORegistryEntryGetName(s, name);
        printf("%s[%d] class=%s name=%s regID=0x%llx\n", cls, n, c, name, (unsigned long long)reg_id(s));
        print_props(s, keys);
        uint64_t pci = print_path_to_pci(s);
        printf("    => owning IOPCIDevice regID=0x%llx\n", (unsigned long long)pci);
        if (out && n < max) out[n] = s; else IOObjectRelease(s);
        n++;
    }
    IOObjectRelease(it);
    printf("%s: %d service(s)\n\n", cls, n);
    return n;
}

int main(int argc, char **argv) {
    int find = 1;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--no-find")) find = 0;
        else { fprintf(stderr, "usage: display-registry [--no-find]\n"); return 2; }
    }
    printf("display-registry: IORegistry reads only (no Metal, no CoreGraphics, no user client)\n\n");
    io_service_t fbs[16] = { 0 };
    int nfb = list_class("IOFramebuffer", kFbKeys, fbs, 16);
    list_class("IOAccelerator", kAccelKeys, NULL, 0);
    if (find) {
        printf("IOAccelFindAccelerator (IOGraphicsLib registry lookup), per framebuffer:\n");
        fflush(stdout);
        for (int i = 0; i < nfb && i < 16; i++) {
            io_service_t acc = IO_OBJECT_NULL; UInt32 idx = 0xffffffffu;
            kern_return_t kr = IOAccelFindAccelerator(fbs[i], &acc, &idx);
            printf("  framebuffer[%d] regID=0x%llx -> kr=0x%x accelerator regID=0x%llx index=%u\n", i,
                   (unsigned long long)reg_id(fbs[i]), kr,
                   (unsigned long long)(acc ? reg_id(acc) : 0), idx);
            if (acc) { io_name_t c = ""; IOObjectGetClass(acc, c); printf("    accelerator class=%s\n", c); IOObjectRelease(acc); }
            fflush(stdout);
        }
    }
    for (int i = 0; i < nfb && i < 16; i++) if (fbs[i]) IOObjectRelease(fbs[i]);
    return 0;
}
