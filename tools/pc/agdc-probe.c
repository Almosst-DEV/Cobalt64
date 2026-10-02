// agdc-probe — route b's first test from userspace display brief).
//
// What it does, in order, every return code printed:
//   1. the accelerator's IOAccelDisplayPipeCapabilities property (AMD's populateAccelConfig publishes it, bdbfc0f; IOPresentment
//      requires DisplayPipeSupported and TransactionsSupported > 0 from the pipe's copy of it, 0x7ff8159916a6-0x7ff81599170c),
//      read as ONE property;
//   2. RDNA4FB's registry ID;
//   3. every service matching AppleGraphicsDeviceControl: class, name, registry ID and its IOService-plane parents up to the
//      first one carrying class-code (what ioPresentmentGetNodeMap walks, 0x7ff81599597a);
//   4. for each that is not AppleGPUWrangler: IOServiceOpen(type 2) as IOPresentment does (0x7ff81598fcf7), and on success
//      kAGDCVendorInfo (selector 1, 0x2c bytes out) and kAGDCGPUCapability (selector 0x980, 0xdc bytes out) with the fields decoded;
//      the AGDC user client admits only tasks entitled com.apple.private.applegraphicsdevicecontrol or com.apple.private.gpuwrangler
//      (authorizeTask 13d3b866 / 13d3b8a1), so this process is EXPECTED to be refused and the refusal is the measurement;
//   5. AppleGPUWrangler's debug-gpus property;
//   6. (ONLY with --iop, see below) IOPresentment::IOPresentmentCreateForRegistryID(0, RDNA4FB) through dlopen of the private
//      framework - the call CoreDisplay makes (0x7ff805368500) - and, if it returns an object, the capability-structure audit
//      of step 7, then IOPresentmentDestroy. Never IOPresentmentInitialize, never a transaction, never
//      the display-pipe user client itself (IOAccelDisplayPipeUserClient2::setPipeIndex needs com.apple.private.hid.client.event-
//      monitor, 145d293d, so CreateForRegistryID could not connect a pipe from here even with a valid AGDC).
//   7. (ONLY with --iop, and only if step 6 returned an object) the capability structure, per notes/M4-CAPABILITIES-STRUCT.md:
//      IOPresentmentCopyCapabilities -> the whole CFDictionary written to --xml <path> as a plist AND audited key by key
//      against every required key of every parser, then IOPresentmentCapabilitiesStructureFromDictionary printed with its
//      code and the 0xb8-byte structure's device count (+0xa8) and plane count (device+0x88), then StructureFree. All three
//      are exported (0x7ff81598dab8 / 0x7ff81598dae5 / 0x7ff81598db2d) and none of them begins a transaction.
//   8. the display state CoreGraphics reports for this process, for the CDSurface+0x40 question (read-only).
//
// *** STEP 6 ABORTS THIS PROCESS WITHOUT AN ENTITLEMENT, and that is why it is now opt-in. ***
// Run `agdc2` (notes/logs/runs/agdc2 died SIGABRT with this backtrace, before printing its result line:
//     abort <- GPUWrangler+22157 <- GPUWrangler+12690 <- GPUWranglerForEachGPU
//           <- IOPresentment`iopGetGPUWranglerResourceDescription() <- block in IOPresentmentCreateForRegistryID
//           <- _dispatch_once_callout <- IOPresentmentCreateForRegistryID <- main
// with `GPUWranglerOpenWithTimeout: failed to connect; service=0x1a33 status=0xe00002e2` (kIOReturnNotPermitted) and
// `AGDCC: Unauthorized client 'agdc-probe' blocked`. GPUWrangler aborts the caller when it cannot open, so steps 6-7 are
// UNREACHABLE from an unentitled task however read-only their intent. The default run therefore stops after step 5 + 8 and
// cannot corpse-dump; --iop is for a build that has been given com.apple.private.applegraphicsdevicecontrol (or
// com.apple.private.gpuwrangler), which this project has not.
//
// Read-only in every mode: property reads, an IOServiceOpen that the kernel refuses, and - under --iop only - capability
// queries. No transaction, no IOPresentmentBegin, no flip, no DCN write, no Metal, no accelerator user client, no reboot.
//
// usage: agdc-probe [--iop] [--xml <path>]
// Run as root, console root, on the armed adopted boot with `pipeguard 1` armed.
//
// Build on the host Mac (tools/stage-to-pc.sh does):
//   clang -arch x86_64 -mmacosx-version-min=12.0 -O2 -Wall -framework IOKit -framework CoreFoundation \
//         tools/pc/agdc-probe.c -o tools/pc/agdc-probe
// (unchanged from tools/stage-to-pc.sh:214 - step 8 reaches CoreGraphics through dlopen precisely so that line
//  does not have to change; stage-to-pc.sh is not this file's owner's to edit.)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <IOKit/IOKitLib.h>
#include <CoreFoundation/CoreFoundation.h>

#define PROBE_TOKEN "agdc-probe: route b first test v2 capstruct"

static uint64_t reg_id(io_registry_entry_t e) { uint64_t id = 0; IORegistryEntryGetRegistryEntryID(e, &id); return id; }

static void print_cf(const char *key, CFTypeRef v) {
    char buf[1024] = "?";
    if (!v) { printf("  %s = <absent>\n", key); return; }
    CFStringRef d = CFCopyDescription(v);
    if (d) { CFStringGetCString(d, buf, sizeof buf, kCFStringEncodingUTF8); CFRelease(d); }
    printf("  %s = %s\n", key, buf);
}

static void hexdump(const unsigned char *p, size_t n) {
    for (size_t i = 0; i < n; i += 16) {
        printf("    +%03zx:", i);
        for (size_t k = i; k < i + 16 && k < n; k++) printf(" %02x", p[k]);
        printf("\n");
    }
}

static uint32_t rd32(const unsigned char *p, size_t off) { uint32_t v; memcpy(&v, p + off, 4); return v; }
static uint64_t rd64(const unsigned char *p, size_t off) { uint64_t v; memcpy(&v, p + off, 8); return v; }

static void parents(io_registry_entry_t e) {
    io_registry_entry_t cur = e; IOObjectRetain(cur);
    for (int hop = 0; hop < 12; hop++) {
        io_registry_entry_t parent = IO_OBJECT_NULL;
        if (IORegistryEntryGetParentEntry(cur, kIOServicePlane, &parent) != KERN_SUCCESS) break;
        IOObjectRelease(cur); cur = parent;
        io_name_t cls = "", name = "";
        IOObjectGetClass(cur, cls); IORegistryEntryGetName(cur, name);
        CFTypeRef cc = IORegistryEntryCreateCFProperty(cur, CFSTR("class-code"), kCFAllocatorDefault, 0);
        uint64_t code = 0;
        if (cc && CFGetTypeID(cc) == CFDataGetTypeID() && CFDataGetLength(cc) > 0)
            memcpy(&code, CFDataGetBytePtr(cc), CFDataGetLength(cc) < 8 ? (size_t)CFDataGetLength(cc) : 8);
        printf("    parent[%d] %-40s name=%-20s regID=%#llx%s", hop, cls, name, (unsigned long long)reg_id(cur), cc ? "" : "\n");
        if (cc) { printf(" class-code=%#llx\n", (unsigned long long)code); CFRelease(cc); break; }
    }
    IOObjectRelease(cur);
}

// ---- step 7 helpers: the capability structure, per notes/M4-CAPABILITIES-STRUCT.md -------------------------------------
// Every key below is one that a parser in IOPresentment demands; the address after it is the `movl $0x2004` it reaches when
// the key is absent, so a MISSING line here names the exact instruction that would return CAPABILITIES_MALFORMED.

static int has_key(CFDictionaryRef d, const char *k) {
    if (!d || CFGetTypeID(d) != CFDictionaryGetTypeID()) return -1;
    CFStringRef s = CFStringCreateWithCString(NULL, k, kCFStringEncodingUTF8);
    int r = s && CFDictionaryContainsKey(d, s);
    if (s) CFRelease(s);
    return r;
}

static CFTypeRef get_key(CFDictionaryRef d, const char *k) {
    if (!d || CFGetTypeID(d) != CFDictionaryGetTypeID()) return NULL;
    CFStringRef s = CFStringCreateWithCString(NULL, k, kCFStringEncodingUTF8);
    CFTypeRef v = s ? CFDictionaryGetValue(d, s) : NULL;
    if (s) CFRelease(s);
    return v;
}

// One required key. Prints present/MISSING and, for the two keys whose *value* is checked, the value too.
static int req(CFDictionaryRef d, const char *k, const char *site, const char *why) {
    int h = has_key(d, k);
    if (h < 0) { printf("      %-28s  (no dictionary)\n", k); return 0; }
    CFTypeRef v = h ? get_key(d, k) : NULL;
    char val[128] = "";
    if (v && CFGetTypeID(v) == CFNumberGetTypeID()) {
        long long n = 0; CFNumberGetValue(v, kCFNumberLongLongType, &n);
        snprintf(val, sizeof val, " = %lld (%#llx)", n, (unsigned long long)n);
    } else if (v && CFGetTypeID(v) == CFDataGetTypeID()) {
        snprintf(val, sizeof val, " = CFData len %#lx", (unsigned long)CFDataGetLength(v));
    } else if (v && CFGetTypeID(v) == CFArrayGetTypeID()) {
        snprintf(val, sizeof val, " = CFArray count %ld", (long)CFArrayGetCount(v));
    }
    printf("      %-28s %-8s%s%s%s\n", k, h ? "present" : "MISSING", val,
           h ? "" : "   <- would return 0x2004 at ", h ? "" : site);
    if (!h && why) printf("          %s\n", why);
    return h;
}

// The pipeline/plane scaler sub-dictionary. __scalerFromDictionary 0x7ff815994782: absent is TOLERATED (returns 0 at
// 0x7ff8159948a6), but present-and-incomplete is 0x2004 at 0x7ff81599489c. The value test that actually decides it is
// Flags bit 0x4 (0x7ff81599481a movb (%rbx),%al; andb $0x4,%al; cmpb $0x1,%al; adcb $0x0,%r15b), and SourceSubRegion,
// when present, must be exactly 0x20 bytes (0x7ff815994848 cmpq $0x20).
static void audit_scaler(CFDictionaryRef parent, const char *key) {
    CFTypeRef s = get_key(parent, key);
    if (!s) { printf("    %s: ABSENT (tolerated - __scalerFromDictionary returns 0 at 0x7ff8159948a6)\n", key); return; }
    printf("    %s: present\n", key);
    CFDictionaryRef d = (CFDictionaryRef)s;
    req(d, "Flags", "0x7ff81599489c", "and Flags must have bit 0x4 SET, or 0x2004 regardless of presence");
    CFTypeRef f = get_key(d, "Flags");
    if (f && CFGetTypeID(f) == CFNumberGetTypeID()) {
        long long n = 0; CFNumberGetValue(f, kCFNumberLongLongType, &n);
        printf("      Flags bit 0x4 %s  <- the decisive test at 0x7ff81599481a\n", (n & 4) ? "SET (ok)" : "CLEAR -> 0x2004");
    }
    req(d, "NonAtomicApplicationPeriod", "0x7ff81599489c", NULL);
    CFTypeRef sr = get_key(d, "SourceSubRegion");
    if (sr && CFGetTypeID(sr) == CFDataGetTypeID())
        printf("      SourceSubRegion              CFData len %#lx %s\n", (unsigned long)CFDataGetLength(sr),
               CFDataGetLength(sr) == 0x20 ? "(ok)" : "-> 0x2004 at 0x7ff815994848 (must be 0x20)");
    else printf("      SourceSubRegion              %s\n", sr ? "present, not CFData" : "absent (tolerated)");
    // Empty arrays are fine: __sourceScalerFromDictionary returns 0 for absent AND for count<=0 (0x7ff81599438a jle).
    for (int i = 0; i < 2; i++) {
        const char *k = i ? "SourceDownScalingArray" : "SourceUpScalingArray";
        CFTypeRef a = get_key(d, k);
        printf("      %-28s %s\n", k, a ? (CFGetTypeID(a) == CFArrayGetTypeID() ? "present" : "present, not CFArray") : "absent (tolerated)");
        if (a && CFGetTypeID(a) == CFArrayGetTypeID()) printf("          count %ld (0 is tolerated at 0x7ff81599438a)\n", (long)CFArrayGetCount(a));
    }
}

static void audit_caps(CFDictionaryRef caps) {
    printf("  [audit] top level\n");
    req(caps, "PipelineDictionaryVersion", "0x7ff815993035 (0x2001)", "expect 0x71 (0x7ff81599a769)");
    req(caps, "APIErrorDictionary", "-", "if present its APIErrorCode is returned verbatim (0x7ff8159930d4)");
    CFDictionaryRef ep = (CFDictionaryRef)get_key(caps, "EndPointDictionary");
    printf("  [audit] EndPointDictionary: %s\n", ep ? "present" : "ABSENT");
    if (ep) {
        static const char *epk[] = { "Flags", "PixelFormats", "LooksLikeWidth", "LooksLikeHeight", "RefreshTime",
            "MinRefreshTime", "MaxRefreshTime", "PixelClock", "MinPixelClock", "MaxPixelClock", "HorizontalActive",
            "HorizontalBlanking", "HorizontalSyncOffset", "HorizontalSyncPulseWidth", "HorizontalSyncConfig",
            "VerticalActive", "VerticalBlanking", "VerticalSyncOffset", "VerticalSyncPulseWidth", "VerticalSyncConfig", NULL };
        for (int i = 0; epk[i]; i++) req(ep, epk[i], "__endPointFromDictionary 0x7ff81599321e..35bb", NULL);
    }
    CFDictionaryRef ib = (CFDictionaryRef)get_key(caps, "InlineBufferDictionary");
    printf("  [audit] InlineBufferDictionary: %s\n", ib ? "present" : "ABSENT");
    if (ib) {
        static const char *ibk[] = { "Flags", "ActivationLatency", "DeactivationLatency", "MaxWidth", "MaxHeight",
                                     "PixelFormats", NULL };
        for (int i = 0; ibk[i]; i++) req(ib, ibk[i], "__inlineBufferFromDictionary 0x7ff8159936c1..3794", NULL);
    }
    CFArrayRef devs = (CFArrayRef)get_key(caps, "DeviceArray");
    if (!devs || CFGetTypeID(devs) != CFArrayGetTypeID()) {
        printf("  [audit] DeviceArray: ABSENT -> 0x2003 at 0x7ff815993801\n"); return;
    }
    long n = (long)CFArrayGetCount(devs);
    printf("  [audit] DeviceArray: count %ld%s\n", n, n <= 0 ? "  -> 0x2002 at 0x7ff81599390a (TOLERATED; CoreDisplay then logs \"capabilities with no devices\")" : "");
    for (long i = 0; i < n; i++) {
        CFDictionaryRef dev = (CFDictionaryRef)CFArrayGetValueAtIndex(devs, i);
        printf("  [audit] device[%ld]\n", i);
        CFDictionaryRef q = (CFDictionaryRef)get_key(dev, "QueueDictionary");
        if (!q) printf("    QueueDictionary: ABSENT (tolerated - 0x2002 at 0x7ff8159941e0)\n");
        else {
            printf("    QueueDictionary: present\n");
            req(q, "Flags", "0x7ff815994251", NULL);
            // QueueDepth is written ONLY by the ...DisplayPipe override (0x7ff8159a0299 movq $0x4); the base class
            // 0x7ff81599b116 writes Flags alone. MISSING here therefore also says which class built the dictionary.
            if (!req(q, "QueueDepth", "0x7ff81599422c", "absent => the BASE IOPresentmentCapabilities built this, not ...DisplayPipe"))
                printf("          => the capabilities object is NOT IOPresentmentCapabilitiesDisplayPipe\n");
        }
        audit_scaler(dev, "PipelineScalerDictionary");
        CFArrayRef planes = (CFArrayRef)get_key(dev, "PlaneArray");
        if (!planes || CFGetTypeID(planes) != CFArrayGetTypeID()) { printf("    PlaneArray: ABSENT -> 0x2003 at 0x7ff815994e27\n"); continue; }
        long pn = (long)CFArrayGetCount(planes);
        printf("    PlaneArray: count %ld%s\n", pn, pn <= 0 ? "  -> 0x2002 at 0x7ff815995088 (CoreDisplay then logs \"device with no planes\")" : "");
        for (long p = 0; p < pn; p++) {
            CFDictionaryRef pl = (CFDictionaryRef)CFArrayGetValueAtIndex(planes, p);
            printf("    plane[%ld]\n", p);
            static const char *plk[] = { "Flags", "BlendType", "Width", "Height", "ClosedCaptionRegions", "PixelFormats", NULL };
            for (int k = 0; plk[k]; k++) req(pl, plk[k], "__planesFromDictionary 0x7ff815994fc4", NULL);
            audit_scaler(pl, "PlaneScalerDictionary");
        }
    }
}

int main(int argc, char **argv) {
    int want_iop = 0;
    const char *xml_path = "/tmp/agdc-caps.plist";
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--iop")) want_iop = 1;
        else if (!strcmp(argv[i], "--xml") && i + 1 < argc) xml_path = argv[++i];
        else { fprintf(stderr, "usage: %s [--iop] [--xml <path>]\n", argv[0]); return 2; }
    }
    printf("%s\n", PROBE_TOKEN);
    printf("mode: step 6/7 (IOPresentment) %s; xml -> %s\n",
           want_iop ? "ENABLED by --iop (WILL ABORT unless this build is entitled; see the header)" : "SKIPPED (default; step 6 corpse-dumped in run agdc2)",
           xml_path);
    // 1. the accelerator's pipe capabilities, one property.
    io_iterator_t it = IO_OBJECT_NULL;
    if (IOServiceGetMatchingServices(kIOMainPortDefault, IOServiceMatching("IOAccelerator"), &it) == KERN_SUCCESS) {
        io_service_t s;
        while ((s = IOIteratorNext(it))) {
            io_name_t cls = ""; IOObjectGetClass(s, cls);
            printf("[accelerator] %s regID %#llx\n", cls, (unsigned long long)reg_id(s));
            CFTypeRef v = IORegistryEntryCreateCFProperty(s, CFSTR("IOAccelDisplayPipeCapabilities"), kCFAllocatorDefault, 0);
            print_cf("IOAccelDisplayPipeCapabilities", v);
            // The two fields IOPresentment reads out of it (0x7ff8159916a6-0x7ff81599170c): both must be > 0 or
            // _IOPresentmentInit gives up before any capability gather happens.
            if (v && CFGetTypeID(v) == CFDictionaryGetTypeID()) {
                long long dps = -1, tts = -1;
                CFTypeRef a = CFDictionaryGetValue((CFDictionaryRef)v, CFSTR("DisplayPipeSupported"));
                CFTypeRef b = CFDictionaryGetValue((CFDictionaryRef)v, CFSTR("TransactionsSupported"));
                if (a && CFGetTypeID(a) == CFNumberGetTypeID()) CFNumberGetValue(a, kCFNumberLongLongType, &dps);
                if (b && CFGetTypeID(b) == CFNumberGetTypeID()) CFNumberGetValue(b, kCFNumberLongLongType, &tts);
                printf("    DisplayPipeSupported = %lld, TransactionsSupported = %lld%s\n", dps, tts,
                       (dps > 0 && tts > 0) ? "" : "   <- IOPresentment requires both > 0");
            }
            if (v) CFRelease(v);
            IOObjectRelease(s);
        }
        IOObjectRelease(it);
    }
    // 2. RDNA4FB.
    // Since 0.0.307 our framebuffer publishes as AMDRDNA4FB when boot-arg rdna4-amdname=1 is set and
    // as RDNA4FB otherwise. Try both, newest name first, and say which answered - a probe
    // that silently found nothing would look exactly like a framebuffer that failed to publish.
    io_service_t fb = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("AMDRDNA4FB"));
    const char *fbClass = "AMDRDNA4FB";
    if (!fb) { fb = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("RDNA4FB")); fbClass = "RDNA4FB"; }
    if (!fb) fbClass = "(neither RDNA4FB nor AMDRDNA4FB)";
    printf("[framebuffer] class %s\n", fbClass);
    uint64_t fbId = fb ? reg_id(fb) : 0;
    printf("[framebuffer] RDNA4FB regID %#llx\n", (unsigned long long)fbId);
    if (fb) { CFTypeRef v = IORegistryEntryCreateCFProperty(fb, CFSTR("IOAccelIndex"), kCFAllocatorDefault, 0); print_cf("IOAccelIndex", v); if (v) CFRelease(v); }
    // 3-4. AGDC services.
    int nAgdc = 0;
    if (IOServiceGetMatchingServices(kIOMainPortDefault, IOServiceMatching("AppleGraphicsDeviceControl"), &it) == KERN_SUCCESS) {
        io_service_t s;
        while ((s = IOIteratorNext(it))) {
            nAgdc++;
            io_name_t cls = "", name = ""; IOObjectGetClass(s, cls); IORegistryEntryGetName(s, name);
            printf("[agdc %d] class %s name %s regID %#llx\n", nAgdc, cls, name, (unsigned long long)reg_id(s));
            parents(s);
            if (!strcmp(name, "AppleGPUWrangler")) { printf("    (skipped as IOPresentment skips it, 0x7ff81598fd86)\n"); IOObjectRelease(s); continue; }
            io_connect_t c = IO_OBJECT_NULL;
            kern_return_t kr = IOServiceOpen(s, mach_task_self(), 2, &c);
            printf("    IOServiceOpen(type 2) kr=%#x%s\n", kr, kr ? " (the AGDC user client's entitlement check is the expected refusal)" : "");
            if (kr == KERN_SUCCESS) {
                unsigned char vi[0x2c] = { 0 }; size_t vl = sizeof vi;
                kr = IOConnectCallMethod(c, 1, NULL, 0, NULL, 0, NULL, NULL, vi, &vl);
                printf("    kAGDCVendorInfo sel 1 kr=%#x len %#zx: +0 %#x vendor %#x type %u\n", kr, vl, rd32(vi, 0), rd32(vi, 0x24), rd32(vi, 0x28));
                hexdump(vi, vl);
                unsigned char cap[0xdc] = { 0 }; size_t cl = sizeof cap;
                kr = IOConnectCallMethod(c, 0x980, NULL, 0, NULL, 0, NULL, NULL, cap, &cl);
                printf("    kAGDCGPUCapability sel 0x980 kr=%#x len %#zx: +0 mask %#llx +0x20 %u +0x30 count %u +0x34 %#llx +0x3c[0] %#llx (RDNA4FB %#llx -> %s)\n",
                       kr, cl, (unsigned long long)rd64(cap, 0), rd32(cap, 0x20), rd32(cap, 0x30), (unsigned long long)rd64(cap, 0x34),
                       (unsigned long long)rd64(cap, 0x3c), (unsigned long long)fbId, rd64(cap, 0x3c) == fbId ? "MATCHES" : "differs");
                hexdump(cap, cl);
                IOServiceClose(c);
            }
            IOObjectRelease(s);
        }
        IOObjectRelease(it);
    }
    printf("[agdc] %d service(s) match AppleGraphicsDeviceControl\n", nAgdc);
    // 5. the wrangler's view.
    io_service_t w = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("AppleGPUWrangler"));
    if (w) { CFTypeRef v = IORegistryEntryCreateCFProperty(w, CFSTR("debug-gpus"), kCFAllocatorDefault, 0); print_cf("AppleGPUWrangler debug-gpus", v); if (v) CFRelease(v); IOObjectRelease(w); }
    // 6-7. IOPresentment. Opt-in only: step 6 aborts an unentitled task inside GPUWrangler (see the header, run agdc2).
    if (!want_iop) {
        printf("[iopresentment] SKIPPED. IOPresentmentCreateForRegistryID reaches GPUWranglerForEachGPU, which calls abort()\n");
        printf("[iopresentment] when GPUWranglerOpenWithTimeout is refused (0xe00002e2). Run agdc2 corpse-dumped there.\n");
        printf("[iopresentment] Pass --iop only from a build entitled com.apple.private.applegraphicsdevicecontrol.\n");
    } else {
        void *h = dlopen("/System/Library/PrivateFrameworks/IOPresentment.framework/IOPresentment", RTLD_NOW);
        printf("[iopresentment] dlopen %p%s%s\n", h, h ? "" : " ", h ? "" : dlerror());
        if (h && fbId) {
            typedef void *(*CreateFn)(uint64_t, uint64_t);
            typedef int (*DestroyFn)(void *);
            typedef CFDictionaryRef (*CopyCapsFn)(void *);
            typedef int (*FromDictFn)(void *, CFDictionaryRef, void *);
            typedef int (*FreeStructFn)(void *, void *);
            // The exported C entry: _IOPresentmentCreateForRegistryID @0x7ff81598da67 calls the C++ one (0x7ff8159910ae) and returns
            // its IOPresentmentRef or NULL.
            CreateFn cf = (CreateFn)dlsym(h, "IOPresentmentCreateForRegistryID");
            DestroyFn df = (DestroyFn)dlsym(h, "IOPresentmentDestroy");
            CopyCapsFn cc = (CopyCapsFn)dlsym(h, "IOPresentmentCopyCapabilities");                        // 0x7ff81598dab8
            FromDictFn fd = (FromDictFn)dlsym(h, "IOPresentmentCapabilitiesStructureFromDictionary");     // 0x7ff81598dae5
            FreeStructFn fs = (FreeStructFn)dlsym(h, "IOPresentmentCapabilitiesStructureFree");           // 0x7ff81598db2d
            printf("[iopresentment] Create %p Destroy %p CopyCapabilities %p StructureFromDictionary %p StructureFree %p\n",
                   (void *)cf, (void *)df, (void *)cc, (void *)fd, (void *)fs);
            if (cf) {
                printf("[iopresentment] calling IOPresentmentCreateForRegistryID(0, %#llx) - this is the abort point\n", (unsigned long long)fbId);
                fflush(stdout);
                void *pr = cf(0, fbId);
                printf("[iopresentment] IOPresentmentCreateForRegistryID(0, %#llx) = %p (%s)\n", (unsigned long long)fbId, pr,
                       pr ? "AN IOPresentment OBJECT - a display pipe would enter CoreDisplay's map" : "NULL - no pipe");
                if (pr && cc) {
                    CFDictionaryRef caps = cc(pr);
                    printf("[capstruct] IOPresentmentCopyCapabilities -> %p%s\n", (const void *)caps,
                           caps ? "" : "  (NULL: _PerformSecondaryCapabilitiesValidation failed, 0x7ff8159a1876 - CoreDisplay would THROW here)");
                    if (caps) {
                        CFDataRef d = CFPropertyListCreateData(NULL, caps, kCFPropertyListXMLFormat_v1_0, 0, NULL);
                        if (d) {
                            FILE *f = fopen(xml_path, "wb");
                            if (f) { fwrite(CFDataGetBytePtr(d), 1, (size_t)CFDataGetLength(d), f); fclose(f);
                                     printf("[capstruct] wrote %ld bytes of plist to %s\n", (long)CFDataGetLength(d), xml_path); }
                            else printf("[capstruct] could not open %s\n", xml_path);
                            CFRelease(d);
                        } else printf("[capstruct] CFPropertyListCreateData failed (a non-plist value in the dictionary)\n");
                        audit_caps(caps);
                        if (fd) {
                            unsigned char st[0xb8]; memset(st, 0, sizeof st);
                            int rc = fd(pr, caps, st);
                            uint64_t ndev = rd64(st, 0xa8), devp = rd64(st, 0xb0);
                            printf("[capstruct] StructureFromDictionary -> %#x  version=%#x deviceCount(+0xa8)=%llu deviceArray(+0xb0)=%#llx\n",
                                   rc, rd32(st, 0), (unsigned long long)ndev, (unsigned long long)devp);
                            // CoreDisplay::LoadCapabilities 0x7ff80536bcc9, verbatim: accepts 0 and 0x2002, then
                            // 0x7ff80536bd1f cmpq $0,0xa8 -> "no devices"; 0x7ff80536bd33 cmpq $0,0x88(device) -> "no planes".
                            if (rc != 0 && rc != 0x2002)
                                printf("[verdict] CoreDisplay would log: \"Failed to get capabilities\" and LoadCapabilities returns FALSE\n");
                            else if (!ndev)
                                printf("[verdict] CoreDisplay would log: \"capabilities with no devices\" and return FALSE\n");
                            else {
                                uint64_t nplanes = devp ? *(uint64_t *)(uintptr_t)(devp + 0x88) : 0;
                                printf("[capstruct] device[0] planeCount(+0x88)=%llu\n", (unsigned long long)nplanes);
                                if (!nplanes) printf("[verdict] CoreDisplay would log: \"device with no planes\" and return FALSE\n");
                                else printf("[verdict] LoadCapabilities would SUCCEED - the capability side really is closed\n");
                            }
                            if ((rc == 0 || rc == 0x2002) && fs) fs(pr, st);
                        }
                        CFRelease(caps);
                    }
                }
                if (pr && df) printf("[iopresentment] IOPresentmentDestroy -> %#x\n", df(pr));
            }
        }
    }
    // 8. what CoreGraphics says about the display from here - context for the CDSurface+0x40 question. A non-WindowServer
    //    process cannot read CDSurface+0x40 itself (it is WindowServer-internal), so this is the reachable half: whether
    //    the display this pipe belongs to is online and active at probe time. Reached by dlopen so that the build line in
    //    tools/stage-to-pc.sh:214 needs no new framework.
    {
        typedef uint32_t CGDisplayID_t;
        typedef int (*ListFn)(uint32_t, CGDisplayID_t *, uint32_t *);
        typedef int (*BoolFn)(CGDisplayID_t);
        typedef uint32_t (*U32Fn)(CGDisplayID_t);
        void *cg = dlopen("/System/Library/Frameworks/CoreGraphics.framework/CoreGraphics", RTLD_NOW);
        ListFn list = cg ? (ListFn)dlsym(cg, "CGGetOnlineDisplayList") : NULL;
        BoolFn on = cg ? (BoolFn)dlsym(cg, "CGDisplayIsOnline") : NULL;
        BoolFn act = cg ? (BoolFn)dlsym(cg, "CGDisplayIsActive") : NULL;
        BoolFn slp = cg ? (BoolFn)dlsym(cg, "CGDisplayIsAsleep") : NULL;
        BoolFn mn = cg ? (BoolFn)dlsym(cg, "CGDisplayIsMain") : NULL;
        U32Fn ven = cg ? (U32Fn)dlsym(cg, "CGDisplayVendorNumber") : NULL;
        U32Fn mod = cg ? (U32Fn)dlsym(cg, "CGDisplayModelNumber") : NULL;
        U32Fn wid = cg ? (U32Fn)dlsym(cg, "CGDisplayPixelsWide") : NULL;
        U32Fn hei = cg ? (U32Fn)dlsym(cg, "CGDisplayPixelsHigh") : NULL;
        CGDisplayID_t ids[8]; uint32_t cnt = 0;
        if (list && list(8, ids, &cnt) == 0) {
            printf("[cgdisplay] %u online display(s)\n", cnt);
            for (uint32_t i = 0; i < cnt; i++)
                printf("    id %#x online=%d active=%d asleep=%d main=%d vendor=%#x model=%#x %ux%u\n",
                       ids[i], on ? on(ids[i]) : -1, act ? act(ids[i]) : -1, slp ? slp(ids[i]) : -1,
                       mn ? mn(ids[i]) : -1, ven ? ven(ids[i]) : 0, mod ? mod(ids[i]) : 0,
                       wid ? wid(ids[i]) : 0, hei ? hei(ids[i]) : 0);
        } else printf("[cgdisplay] unavailable (dlopen %p, CGGetOnlineDisplayList %p)\n", cg, (void *)list);
    }
    if (fb) IOObjectRelease(fb);
    printf("agdc-probe: done\n");
    return 0;
}
