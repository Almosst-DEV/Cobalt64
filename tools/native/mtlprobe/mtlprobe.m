// mtlprobe: Metal device list + offscreen golden triangle, for native milestones #9/#10.
// Build: see build.sh. Usage: mtlprobe list | mtlprobe triangle <out.png> [--registry-id N] [--precompiled [file.metallib]] [--expected file.raw]
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <ImageIO/ImageIO.h>
#import <CoreGraphics/CoreGraphics.h>
#import <IOKit/IOKitLib.h>
#import <IOSurface/IOSurface.h>
#import <objc/runtime.h>
#import <objc/message.h>
#import <CommonCrypto/CommonDigest.h>
#include <sys/stat.h>
#include <math.h>
#include <dlfcn.h>

#define W 256
#define H 256

static const char *kShaderSrc =
"#include <metal_stdlib>\n"
"using namespace metal;\n"
"vertex float4 vs(uint vid [[vertex_id]]) {\n"
"    float2 p[3] = { float2(-0.8, 0.8), float2(0.8, 0.8), float2(0.0, -0.8) };\n"
"    return float4(p[vid], 0.0, 1.0);\n"
"}\n"
"fragment float4 fs() { return float4(1.0, 0.8, 0.0, 1.0); }\n";

static void perr(const char *what, NSError *e) {
    if (e) printf("mtlprobe: %s NSError: %s\n", what, [[e description] UTF8String]);
}

static void print_chain(id obj) {
    Class c = object_getClass(obj);
    printf("    class chain:");
    while (c) { printf(" %s", class_getName(c)); c = class_getSuperclass(c); if (c) printf(" <-"); }
    printf("\n");
}

static void print_ioreg(uint64_t rid) {
    CFMutableDictionaryRef m = IORegistryEntryIDMatching(rid);
    if (!m) { printf("    ioreg: IORegistryEntryIDMatching(0x%llx) returned NULL\n", rid); return; }
    io_service_t s = IOServiceGetMatchingService(kIOMainPortDefault, m); // consumes m
    if (!s) { printf("    ioreg: no service for registryID 0x%llx\n", rid); return; }
    io_name_t cls = {0}, nm = {0}; io_string_t path = {0};
    IOObjectGetClass(s, cls);
    IORegistryEntryGetName(s, nm);
    if (IORegistryEntryGetPath(s, kIOServicePlane, path) != KERN_SUCCESS) strcpy(path, "(no path)");
    printf("    ioreg: class=%s name=%s\n    ioreg path: %s\n", cls, nm, path);
    const char *keys[] = { "MetalPluginName", "MetalPluginClassName", "IOGLBundleName", "MetalStatisticsName",
                           "model", "vendor-id", "device-id", "IOClass", "CFBundleIdentifier", "IOProviderClass" };
    for (size_t i = 0; i < sizeof keys / sizeof *keys; i++) {
        CFTypeRef v = IORegistryEntryCreateCFProperty(s, (__bridge CFStringRef)@(keys[i]), kCFAllocatorDefault, 0);
        if (!v) continue;
        NSString *d;
        if (CFGetTypeID(v) == CFDataGetTypeID()) {
            NSData *dd = (__bridge NSData *)v; const uint8_t *b = dd.bytes; NSMutableString *h = [NSMutableString stringWithString:@"<"];
            for (NSUInteger k = 0; k < dd.length; k++) [h appendFormat:@"%02x", b[k]];
            [h appendString:@">"]; d = h;
        } else d = [(__bridge id)v description];
        printf("    ioreg %s = %s\n", keys[i], [d UTF8String]);
        CFRelease(v);
    }
    // walk parents so a kext service shows up even when the device entry is an accelerator child
    io_registry_entry_t cur = s; int depth = 0;
    IOObjectRetain(cur);
    while (depth < 6) {
        io_registry_entry_t par = 0;
        if (IORegistryEntryGetParentEntry(cur, kIOServicePlane, &par) != KERN_SUCCESS) break;
        io_name_t pc = {0}; IOObjectGetClass(par, pc);
        printf("    ioreg parent[%d]: %s\n", depth, pc);
        IOObjectRelease(cur); cur = par; depth++;
    }
    IOObjectRelease(cur);
    IOObjectRelease(s);
}

static void describe(id<MTLDevice> d, const char *tag) {
    printf("%s: %s\n", tag, [d.name UTF8String]);
    printf("    registryID=0x%llx (%llu)\n", d.registryID, d.registryID);
    const char *loc = "?";
    switch (d.location) { case MTLDeviceLocationBuiltIn: loc = "BuiltIn"; break; case MTLDeviceLocationSlot: loc = "Slot"; break;
        case MTLDeviceLocationExternal: loc = "External"; break; case MTLDeviceLocationUnspecified: loc = "Unspecified"; break; }
    printf("    location=%s locationNumber=%lu\n", loc, (unsigned long)d.locationNumber);
    printf("    isLowPower=%d isHeadless=%d isRemovable=%d hasUnifiedMemory=%d\n", d.isLowPower, d.isHeadless, d.isRemovable, d.hasUnifiedMemory);
    printf("    recommendedMaxWorkingSetSize=%llu maxBufferLength=%lu\n", d.recommendedMaxWorkingSetSize, (unsigned long)d.maxBufferLength);
    struct { const char *n; MTLGPUFamily f; } fam[] = {
        {"Apple1",MTLGPUFamilyApple1},{"Apple2",MTLGPUFamilyApple2},{"Apple3",MTLGPUFamilyApple3},{"Apple4",MTLGPUFamilyApple4},
        {"Apple5",MTLGPUFamilyApple5},{"Apple6",MTLGPUFamilyApple6},{"Apple7",MTLGPUFamilyApple7},{"Apple8",MTLGPUFamilyApple8},
        {"Apple9",MTLGPUFamilyApple9},{"Mac1",MTLGPUFamilyMac1},{"Mac2",MTLGPUFamilyMac2},
        {"Common1",MTLGPUFamilyCommon1},{"Common2",MTLGPUFamilyCommon2},{"Common3",MTLGPUFamilyCommon3},{"Metal3",MTLGPUFamilyMetal3} };
    printf("    supportsFamily:");
    for (size_t i = 0; i < sizeof fam / sizeof *fam; i++) printf(" %s=%d", fam[i].n, [d supportsFamily:fam[i].f]);
    printf("\n");
    print_chain(d);
    print_ioreg(d.registryID);
}

static int cmd_list(void) {
    NSArray<id<MTLDevice>> *all = MTLCopyAllDevices();
    printf("mtlprobe: MTLCopyAllDevices returned %lu device(s)\n", (unsigned long)all.count);
    for (NSUInteger i = 0; i < all.count; i++) { char t[32]; snprintf(t, sizeof t, "device[%lu]", (unsigned long)i); describe(all[i], t); }
    id<MTLDevice> def = MTLCreateSystemDefaultDevice();
    if (def) describe(def, "default"); else printf("mtlprobe: MTLCreateSystemDefaultDevice returned nil\n");
    return all.count ? 0 : 1;
}

static int write_png(const char *path, const uint8_t *bgra) {
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGDataProviderRef dp = CGDataProviderCreateWithData(NULL, bgra, W * H * 4, NULL);
    CGImageRef img = CGImageCreate(W, H, 8, 32, W * 4, cs, kCGBitmapByteOrder32Little | kCGImageAlphaNoneSkipFirst, dp, NULL, false, kCGRenderingIntentDefault);
    int ok = 0;
    if (img) {
        NSURL *u = [NSURL fileURLWithPath:@(path)];
        CGImageDestinationRef dst = CGImageDestinationCreateWithURL((__bridge CFURLRef)u, CFSTR("public.png"), 1, NULL);
        if (dst) { CGImageDestinationAddImage(dst, img, NULL); ok = CGImageDestinationFinalize(dst); CFRelease(dst); }
        CGImageRelease(img);
    }
    CGDataProviderRelease(dp); CGColorSpaceRelease(cs);
    return ok;
}

static int cmd_triangle(const char *out, uint64_t rid, int haveRid, BOOL precompiled, const char *metallib, const char *expPath) {
    id<MTLDevice> dev = nil;
    if (haveRid) {
        for (id<MTLDevice> d in MTLCopyAllDevices()) if (d.registryID == rid) dev = d;
        if (!dev) { printf("mtlprobe: FAIL no device with registryID %llu\n", rid); return 2; }
    } else dev = MTLCreateSystemDefaultDevice();
    if (!dev) { printf("mtlprobe: FAIL no default Metal device\n"); return 2; }
    printf("mtlprobe: using device %s registryID=0x%llx\n", [dev.name UTF8String], dev.registryID);

    NSError *err = nil; id<MTLLibrary> lib = nil;
    if (precompiled) {
        NSString *p = @(metallib);
        printf("mtlprobe: loading precompiled %s\n", metallib);
        lib = [dev newLibraryWithURL:[NSURL fileURLWithPath:p] error:&err];
        perr("newLibraryWithURL", err);
        if (!lib) { printf("mtlprobe: SKIP precompiled library unavailable (build.sh skips it when xcrun metal is missing)\n"); return 3; }
    } else {
        printf("mtlprobe: compiling shader source at runtime\n");
        lib = [dev newLibraryWithSource:@(kShaderSrc) options:nil error:&err];
        perr("newLibraryWithSource", err);
        printf("mtlprobe: step library: %s (%p)\n", lib ? "non-nil" : "NIL", (__bridge void *)lib);
        if (lib) { printf("mtlprobe: step library class: %s; functionNames:", class_getName(object_getClass(lib)));
            for (NSString *n in lib.functionNames) printf(" %s", [n UTF8String]); printf("\n"); }
        if (!lib) { printf("mtlprobe: FAIL library compile\n"); return 2; }
    }
    id<MTLFunction> vs = [lib newFunctionWithName:@"vs"], fs = [lib newFunctionWithName:@"fs"];
    printf("mtlprobe: step functions: vs=%s fs=%s\n", vs ? "non-nil" : "NIL", fs ? "non-nil" : "NIL");
    if (!vs || !fs) { printf("mtlprobe: FAIL functions vs=%p fs=%p\n", (__bridge void *)vs, (__bridge void *)fs); return 2; }
    MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
    pd.vertexFunction = vs; pd.fragmentFunction = fs;
    pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    err = nil; id<MTLRenderPipelineState> pso = [dev newRenderPipelineStateWithDescriptor:pd error:&err];
    perr("newRenderPipelineState", err);
    printf("mtlprobe: step pipeline: %s; error text: %s\n", pso ? "non-nil" : "NIL", err ? [[err localizedDescription] UTF8String] : "(none)");
    if (!pso) { printf("mtlprobe: FAIL pipeline\n"); return 2; }
    printf("mtlprobe: pipeline created\n");

    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:W height:H mipmapped:NO];
    td.usage = MTLTextureUsageRenderTarget; td.storageMode = MTLStorageModePrivate;
    id<MTLTexture> tex = [dev newTextureWithDescriptor:td];
    id<MTLBuffer> buf = [dev newBufferWithLength:W * H * 4 options:MTLResourceStorageModeShared];
    if (!tex || !buf) { printf("mtlprobe: FAIL alloc tex=%p buf=%p\n", (__bridge void *)tex, (__bridge void *)buf); return 2; }
    memset(buf.contents, 0xEE, W * H * 4);
    id<MTLCommandQueue> q = [dev newCommandQueue];
    id<MTLCommandBuffer> cb = q ? [q commandBuffer] : nil;
    if (!cb) { printf("mtlprobe: FAIL command queue/buffer\n"); return 2; }
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = tex;
    rp.colorAttachments[0].loadAction = MTLLoadActionClear;
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    rp.colorAttachments[0].clearColor = MTLClearColorMake(0.0, 0.2, 0.4, 1.0);
    id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:rp];
    if (!re) { printf("mtlprobe: FAIL render encoder\n"); return 2; }
    [re setRenderPipelineState:pso];
    [re setCullMode:MTLCullModeNone];
    [re drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [re endEncoding];
    id<MTLBlitCommandEncoder> be = [cb blitCommandEncoder];
    [be copyFromTexture:tex sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(W,H,1)
               toBuffer:buf destinationOffset:0 destinationBytesPerRow:W * 4 destinationBytesPerImage:W * H * 4];
    [be endEncoding];
    printf("mtlprobe: committing\n");
    [cb commit]; [cb waitUntilCompleted];
    printf("mtlprobe: command buffer status=%ld\n", (long)cb.status);
    if (cb.error) perr("commandBuffer", cb.error);
    if (cb.status != MTLCommandBufferStatusCompleted) { printf("mtlprobe: FAIL command buffer did not complete\n"); return 2; }

    const uint8_t *px = buf.contents;
    if (write_png(out, px)) printf("mtlprobe: wrote %s\n", out); else printf("mtlprobe: WARN PNG write failed for %s\n", out);
    NSData *exp = [NSData dataWithContentsOfFile:@(expPath)];
    if (!exp || exp.length != W * H * 4) { printf("mtlprobe: FAIL cannot read expected %s (%lu bytes)\n", expPath, (unsigned long)exp.length); return 2; }
    const uint8_t *e = exp.bytes; int diff = 0, edge = 0;
    for (int i = 0; i < W * H; i++) {
        // Metal readback is BGRA; expected.raw is RGBA
        uint8_t got[4] = { px[i*4+2], px[i*4+1], px[i*4+0], px[i*4+3] };
        if (memcmp(got, e + i * 4, 4)) diff++;
    }
    (void)edge;
    const uint8_t *c = px + (128 * W + 128) * 4;
    printf("mtlprobe: centre (128,128) RGBA = %u,%u,%u,%u; corner (0,0) RGBA = %u,%u,%u,%u\n", c[2], c[1], c[0], c[3], px[2], px[1], px[0], px[3]);
    printf("mtlprobe: %d of 65536 pixels differ\n", diff);
    if (diff == 0) { printf("mtlprobe: PASS (exact)\n"); return 0; }
    if (diff * 1000 <= 65536 * 5 / 1) { // <= 0.5 percent
        printf("mtlprobe: PASS (with flag: %d differing pixels = %.3f%% <= 0.5%% edge tolerance)\n", diff, diff * 100.0 / 65536); return 0;
    }
    printf("mtlprobe: FAIL\n"); return 1;
}

// mtlprobe buf: newBufferWithLength 4096 Shared, write/read back an 0xEE pattern (native #10 step 10a).
static int cmd_buf(uint64_t rid, int haveRid) {
    id<MTLDevice> dev = nil;
    if (haveRid) {
        for (id<MTLDevice> d in MTLCopyAllDevices()) if (d.registryID == rid) dev = d;
        if (!dev) { printf("mtlprobe: FAIL no device with registryID 0x%llx\n", rid); return 2; }
    } else dev = MTLCreateSystemDefaultDevice();
    if (!dev) { printf("mtlprobe: FAIL no default Metal device\n"); return 2; }
    printf("mtlprobe: using device %s registryID=0x%llx\n", [dev.name UTF8String], dev.registryID);
    id<MTLBuffer> b = [dev newBufferWithLength:4096 options:MTLResourceStorageModeShared];
    printf("mtlprobe: step newBuffer: %s (%p)\n", b ? "non-nil" : "NIL", (__bridge void *)b);
    if (!b) { printf("mtlprobe: FAIL buffer\n"); return 2; }
    print_chain(b);
    printf("mtlprobe: length=%lu contents=%p storageMode=%lu resourceOptions=0x%lx\n", (unsigned long)b.length, b.contents,
           (unsigned long)b.storageMode, (unsigned long)b.resourceOptions);
    if (b.length != 4096 || !b.contents) { printf("mtlprobe: FAIL length/contents\n"); return 2; }
    uint8_t *p = b.contents; memset(p, 0xEE, 4096);
    int bad = 0; for (int i = 0; i < 4096; i++) if (p[i] != 0xEE) bad++;
    for (int i = 0; i < 4096; i++) p[i] = (uint8_t)(i * 7 + 3);
    for (int i = 0; i < 4096; i++) if (p[i] != (uint8_t)(i * 7 + 3)) bad++;
    printf("mtlprobe: buffer round-trip: %d mismatching bytes\n", bad);
    printf(bad ? "mtlprobe: FAIL buf\n" : "mtlprobe: PASS buf\n");
    return bad ? 1 : 0;
}


static id<MTLDevice> pick_device(uint64_t rid, int haveRid) {
    id<MTLDevice> dev = nil;
    if (haveRid) { for (id<MTLDevice> d in MTLCopyAllDevices()) if (d.registryID == rid) dev = d; }
    else dev = MTLCreateSystemDefaultDevice();
    if (!dev) printf("mtlprobe: FAIL no device (registryID 0x%llx)\n", rid);
    else printf("mtlprobe: using device %s registryID=0x%llx\n", [dev.name UTF8String], dev.registryID);
    return dev;
}

// mtlprobe commit: empty command buffer -> commit -> waitUntilCompleted (native #10 step 10b).
static int cmd_commit(uint64_t rid, int haveRid) {
    id<MTLDevice> dev = pick_device(rid, haveRid); if (!dev) return 2;
    id<MTLCommandQueue> q = [dev newCommandQueue];
    printf("mtlprobe: step queue: %s (%p)\n", q ? "non-nil" : "NIL", (__bridge void *)q);
    if (!q) return 2;
    print_chain(q);
    id<MTLCommandBuffer> cb = [q commandBuffer];
    printf("mtlprobe: step commandBuffer: %s (%p)\n", cb ? "non-nil" : "NIL", (__bridge void *)cb);
    if (!cb) return 2;
    print_chain(cb);
    __block int fired = 0, scheduled = 0;
    [cb addScheduledHandler:^(id<MTLCommandBuffer> c) { (void)c; scheduled = 1; }];
    [cb addCompletedHandler:^(id<MTLCommandBuffer> c) { fired = 1; printf("mtlprobe: completed handler fired, status=%ld\n", (long)c.status); }];
    printf("mtlprobe: committing (status before = %ld)\n", (long)cb.status);
    [cb commit]; [cb waitUntilCompleted];
    printf("mtlprobe: command buffer status=%ld (4 = Completed) completedHandlerFired=%d scheduledHandlerFired=%d\n", (long)cb.status, fired, scheduled);
    if (cb.error) perr("commandBuffer", cb.error);
    int ok = cb.status == MTLCommandBufferStatusCompleted && fired && !cb.error;
    printf(ok ? "mtlprobe: PASS commit\n" : "mtlprobe: FAIL commit\n");
    return ok ? 0 : 1;
}

// mtlprobe clear: clear-only render pass on a 256x256 BGRA target, blit to a Shared buffer, expect every pixel (0,51,102,255) (step 10c).
static int cmd_clear(uint64_t rid, int haveRid, const char *out) {
    id<MTLDevice> dev = pick_device(rid, haveRid); if (!dev) return 2;
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:W height:H mipmapped:NO];
    td.usage = MTLTextureUsageRenderTarget; td.storageMode = MTLStorageModePrivate;
    id<MTLTexture> tex = [dev newTextureWithDescriptor:td];
    id<MTLBuffer> buf = [dev newBufferWithLength:W * H * 4 options:MTLResourceStorageModeShared];
    printf("mtlprobe: step texture: %s buffer: %s\n", tex ? "non-nil" : "NIL", buf ? "non-nil" : "NIL");
    if (!tex || !buf) return 2;
    print_chain(tex);
    memset(buf.contents, 0xEE, W * H * 4);
    id<MTLCommandQueue> q = [dev newCommandQueue];
    id<MTLCommandBuffer> cb = q ? [q commandBuffer] : nil;
    if (!cb) { printf("mtlprobe: FAIL queue/commandBuffer\n"); return 2; }
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = tex;
    rp.colorAttachments[0].loadAction = MTLLoadActionClear;
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    rp.colorAttachments[0].clearColor = MTLClearColorMake(0.0, 0.2, 0.4, 1.0);
    id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:rp];
    printf("mtlprobe: step render encoder: %s\n", re ? "non-nil" : "NIL");
    if (!re) return 2;
    [re endEncoding];
    id<MTLBlitCommandEncoder> be = [cb blitCommandEncoder];
    printf("mtlprobe: step blit encoder: %s\n", be ? "non-nil" : "NIL");
    if (!be) return 2;
    [be copyFromTexture:tex sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(W,H,1)
               toBuffer:buf destinationOffset:0 destinationBytesPerRow:W * 4 destinationBytesPerImage:W * H * 4];
    [be endEncoding];
    printf("mtlprobe: committing\n");
    [cb commit]; [cb waitUntilCompleted];
    printf("mtlprobe: command buffer status=%ld\n", (long)cb.status);
    if (cb.error) perr("commandBuffer", cb.error);
    if (cb.status != MTLCommandBufferStatusCompleted) { printf("mtlprobe: FAIL command buffer did not complete\n"); return 1; }
    const uint8_t *px = buf.contents;
    if (out && write_png(out, px)) printf("mtlprobe: wrote %s\n", out);
    int diff = 0;
    for (int i = 0; i < W * H; i++) {
        uint8_t got[4] = { px[i*4+2], px[i*4+1], px[i*4+0], px[i*4+3] };   // BGRA -> RGBA
        static const uint8_t want[4] = { 0, 51, 102, 255 };
        if (memcmp(got, want, 4)) diff++;
    }
    printf("mtlprobe: pixel (0,0) RGBA = %u,%u,%u,%u; (128,128) RGBA = %u,%u,%u,%u\n", px[2], px[1], px[0], px[3],
           px[(128*W+128)*4+2], px[(128*W+128)*4+1], px[(128*W+128)*4+0], px[(128*W+128)*4+3]);
    printf("mtlprobe: %d of 65536 pixels differ from (0,51,102,255)\n", diff);
    printf(diff ? "mtlprobe: FAIL clear\n" : "mtlprobe: PASS clear\n");
    return diff ? 1 : 0;
}

// ---- 10e (m10-2): frames / timeout modes ---------------------------------------------------------------------
#include <mach/mach_time.h>
#include <dispatch/dispatch.h>
static double now_s(void) { static mach_timebase_info_data_t tb; if (!tb.denom) mach_timebase_info(&tb);
    return (double)mach_absolute_time() * tb.numer / tb.denom / 1e9; }

static id<MTLRenderPipelineState> make_pso(id<MTLDevice> dev) {
    NSError *err = nil; id<MTLLibrary> lib = [dev newLibraryWithSource:@(kShaderSrc) options:nil error:&err];
    perr("newLibraryWithSource", err);
    id<MTLFunction> vs = lib ? [lib newFunctionWithName:@"vs"] : nil, fs = lib ? [lib newFunctionWithName:@"fs"] : nil;
    if (!vs || !fs) { printf("mtlprobe: FAIL library/functions\n"); return nil; }
    MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
    pd.vertexFunction = vs; pd.fragmentFunction = fs; pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    err = nil; id<MTLRenderPipelineState> pso = [dev newRenderPipelineStateWithDescriptor:pd error:&err];
    perr("newRenderPipelineState", err);
    if (!pso) printf("mtlprobe: FAIL pipeline\n");
    return pso;
}
static NSData *g_exp;
static int load_expected(void) {
    NSString *dir = [[[NSProcessInfo processInfo] arguments][0] stringByDeletingLastPathComponent]; if (![dir length]) dir = @".";
    NSString *d = [[NSURL fileURLWithPath:dir] URLByResolvingSymlinksInPath].path;
    g_exp = [NSData dataWithContentsOfFile:[d stringByAppendingString:@"/expected.raw"]];
    if (!g_exp || g_exp.length != W * H * 4) { printf("mtlprobe: FAIL cannot read expected.raw\n"); return 0; }
    return 1;
}
static int px_diff(const uint8_t *px) {
    const uint8_t *e = g_exp.bytes; int diff = 0;
    for (int i = 0; i < W * H; i++) { uint8_t got[4] = { px[i*4+2], px[i*4+1], px[i*4+0], px[i*4+3] }; if (memcmp(got, e + i * 4, 4)) diff++; }
    return diff;
}
// One frame: clear + draw `draws` times (one render encoder) + blit to buf.
static id<MTLCommandBuffer> encode_frame(id<MTLCommandQueue> q, id<MTLRenderPipelineState> pso, id<MTLTexture> tex, id<MTLBuffer> buf, int passes) {
    id<MTLCommandBuffer> cb = [q commandBuffer]; if (!cb) return nil;
    for (int k = 0; k < passes; k++) {
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = tex; rp.colorAttachments[0].loadAction = MTLLoadActionClear;
        rp.colorAttachments[0].storeAction = MTLStoreActionStore; rp.colorAttachments[0].clearColor = MTLClearColorMake(0.0, 0.2, 0.4, 1.0);
        id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:rp]; if (!re) return nil;
        [re setRenderPipelineState:pso]; [re setCullMode:MTLCullModeNone];
        [re drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3]; [re endEncoding];
    }
    id<MTLBlitCommandEncoder> be = [cb blitCommandEncoder]; if (!be) return nil;
    [be copyFromTexture:tex sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(W,H,1)
               toBuffer:buf destinationOffset:0 destinationBytesPerRow:W * 4 destinationBytesPerImage:W * H * 4];
    [be endEncoding];
    return cb;
}
static id<MTLTexture> make_target(id<MTLDevice> dev) {
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:W height:H mipmapped:NO];
    td.usage = MTLTextureUsageRenderTarget; td.storageMode = MTLStorageModePrivate;
    return [dev newTextureWithDescriptor:td];
}

// mtlprobe frames N [--inflight K]: one device/queue/pipeline, N command buffers each clear+draw+blit.
static int cmd_frames(uint64_t rid, int haveRid, int N, int inflight) {
    id<MTLDevice> dev = pick_device(rid, haveRid); if (!dev) return 2;
    if (!load_expected()) return 2;
    id<MTLRenderPipelineState> pso = make_pso(dev); if (!pso) return 2;
    id<MTLTexture> tex = make_target(dev);
    id<MTLBuffer> buf = [dev newBufferWithLength:W * H * 4 options:MTLResourceStorageModeShared];
    id<MTLCommandQueue> q = [dev newCommandQueue];
    if (!tex || !buf || !q) { printf("mtlprobe: FAIL alloc tex=%p buf=%p q=%p\n", (__bridge void *)tex, (__bridge void *)buf, (__bridge void *)q); return 2; }
    memset(buf.contents, 0xEE, W * H * 4);
    __block int spotBad = 0, spotN = 0, errs = 0, done = 0;
    dispatch_semaphore_t sem = inflight > 0 ? dispatch_semaphore_create(inflight) : NULL;
    NSLock *lk = [NSLock new];
    double t0 = now_s(); id<MTLCommandBuffer> last = nil;
    for (int i = 0; i < N; i++) {
        if (sem) dispatch_semaphore_wait(sem, DISPATCH_TIME_FOREVER);
        id<MTLCommandBuffer> cb = encode_frame(q, pso, tex, buf, 1);
        if (!cb) { printf("mtlprobe: FAIL encode frame %d\n", i); return 2; }
        int idx = i; const uint8_t *px = buf.contents;
        [cb addCompletedHandler:^(id<MTLCommandBuffer> c) {
            int bad = 0, chk = 0;
            if (c.status != MTLCommandBufferStatusCompleted) { [lk lock]; errs++; [lk unlock]; }
            else if (idx % 10 == 0) { bad = px_diff(px); chk = 1; }
            [lk lock]; done++; if (chk) { spotN++; spotBad += bad; } [lk unlock];
            if (sem) dispatch_semaphore_signal(sem);
        }];
        [cb commit]; last = cb;
    }
    [last waitUntilCompleted];
    double t1 = now_s();
    // the last handler may still be running on the completion queue; drain by waiting for done == N (bounded)
    for (int w = 0; w < 5000; w++) { [lk lock]; int d = done; [lk unlock]; if (d >= N) break; usleep(1000); }
    printf("mtlprobe: frames=%d inflight=%d wall=%.3f s => %.1f frames/s (%.2f ms/frame); last status=%ld; failed cbs=%d; spot-checked %d frames, %d bad pixels\n",
           N, inflight, t1 - t0, N / (t1 - t0), (t1 - t0) * 1000.0 / N, (long)last.status, errs, spotN, spotBad);
    if (last.error) perr("last cb", last.error);
    int diff = px_diff(buf.contents);
    printf("mtlprobe: %d of 65536 pixels differ\n", diff);
    int ok = !diff && !errs && !spotBad && last.status == MTLCommandBufferStatusCompleted;
    printf(ok ? "mtlprobe: PASS frames (exact)\n" : "mtlprobe: FAIL frames\n");
    return ok ? 0 : 1;
}

// mtlprobe timeout [--passes P]: cb A = P render passes + blit (busy GPU); with N48M_TEST_TIMEOUT_MS set the bundle's first fence
// wait is 1 slice of that many ms, so A must reach status Error with an NSError while the GPU is still busy. Then cb B (a normal
// frame, queued behind A) must complete exactly: the bundle's serial completion queue only reaches B after A's fence has signalled.
static int cmd_timeout(uint64_t rid, int haveRid, int passes) {
    id<MTLDevice> dev = pick_device(rid, haveRid); if (!dev) return 2;
    if (!load_expected()) return 2;
    id<MTLRenderPipelineState> pso = make_pso(dev); if (!pso) return 2;
    id<MTLTexture> tex = make_target(dev);
    id<MTLBuffer> buf = [dev newBufferWithLength:W * H * 4 options:MTLResourceStorageModeShared];
    id<MTLCommandQueue> q = [dev newCommandQueue];
    if (!tex || !buf || !q) { printf("mtlprobe: FAIL alloc\n"); return 2; }
    memset(buf.contents, 0xEE, W * H * 4);
    printf("mtlprobe: N48M_TEST_TIMEOUT_MS=%s passes=%d\n", getenv("N48M_TEST_TIMEOUT_MS") ?: "(unset)", passes);
    id<MTLCommandBuffer> A = encode_frame(q, pso, tex, buf, passes);
    id<MTLCommandBuffer> B = encode_frame(q, pso, tex, buf, 1);
    if (!A || !B) { printf("mtlprobe: FAIL encode\n"); return 2; }
    double t0 = now_s(); [A commit]; [B commit];
    [A waitUntilCompleted]; double tA = now_s();
    printf("mtlprobe: A status=%ld (5 = Error) after %.4f s\n", (long)A.status, tA - t0);
    if (A.error) perr("A", A.error);
    [B waitUntilCompleted]; double tB = now_s();
    printf("mtlprobe: B status=%ld (4 = Completed) after %.4f s (GPU finished A and B)\n", (long)B.status, tB - t0);
    if (B.error) perr("B", B.error);
    int diff = px_diff(buf.contents);
    printf("mtlprobe: %d of 65536 pixels differ\n", diff);
    int expectErr = getenv("N48M_TEST_TIMEOUT_MS") != NULL;
    int ok = (expectErr ? (A.status == MTLCommandBufferStatusError && A.error != nil) : A.status == MTLCommandBufferStatusCompleted)
             && B.status == MTLCommandBufferStatusCompleted && !diff;
    printf(ok ? "mtlprobe: PASS timeout (A error path ok, B exact)\n" : "mtlprobe: FAIL timeout\n");
    return ok ? 0 : 1;
}

// mtlprobe libhash <path.metallib> [--registry-id N]: newLibraryWithURL:, then for every functionNames entry
// newFunctionWithName: and print name, stage and sha256(bitcodeData) (the bundle's spvcache key, NATIVE-S4-M10 10a).
// Output lines: "libhash: fn <idx> <name> stage=<vertex|fragment|kernel|other> bytes=<n> sha256=<hex>". Creates no pipeline.
static int cmd_libhash(const char *path, uint64_t rid, int haveRid) {
    id<MTLDevice> dev = pick_device(rid, haveRid); if (!dev) return 2;
    NSError *err = nil;
    NSURL *url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:path]];
    id<MTLLibrary> lib = [dev newLibraryWithURL:url error:&err];
    printf("libhash: path %s\n", path);
    if (err) perr("newLibraryWithURL", err);
    if (!lib) { printf("libhash: FAIL library is nil\n"); return 2; }
    printf("libhash: library class %s, installName %s, type %ld\n", class_getName(object_getClass(lib)),
           lib.installName ? [lib.installName UTF8String] : "(nil)", (long)lib.type);
    NSArray<NSString *> *names = lib.functionNames;
    printf("libhash: functionNames %lu\n", (unsigned long)names.count);
    int nfn = 0, nbc = 0, nnil = 0; SEL bsel = NSSelectorFromString(@"bitcodeData");
    for (NSUInteger i = 0; i < names.count; i++) {
        @autoreleasepool {
            id<MTLFunction> fn = [lib newFunctionWithName:names[i]];
            if (!fn) { printf("libhash: fn %lu %s NIL\n", (unsigned long)i, [names[i] UTF8String]); nnil++; continue; }
            nfn++;
            NSData *bc = [(id)fn respondsToSelector:bsel] ? ((NSData *(*)(id, SEL))objc_msgSend)(fn, bsel) : nil;
            long st = (long)fn.functionType;
            const char *sn = st == 1 ? "vertex" : st == 2 ? "fragment" : st == 3 ? "kernel" : "other";
            if (!bc.length) { printf("libhash: fn %lu %s stage=%s NO_BITCODE\n", (unsigned long)i, [names[i] UTF8String], sn); continue; }
            unsigned char h[CC_SHA256_DIGEST_LENGTH]; CC_SHA256(bc.bytes, (CC_LONG)bc.length, h);
            char hex[65]; for (int k = 0; k < 32; k++) snprintf(hex + 2 * k, 3, "%02x", h[k]);
            printf("libhash: fn %lu %s stage=%s bytes=%lu sha256=%s\n", (unsigned long)i, [names[i] UTF8String], sn, (unsigned long)bc.length, hex);
            nbc++;
        }
    }
    printf("libhash: functions %d, with bitcode %d, nil %d\n", nfn, nbc, nnil);
    printf("libhash: DONE\n");
    return (nfn == (int)names.count && nbc == nfn) ? 0 : 1;
}

// mtlprobe syspipe <metallib> <vertexName> <fragmentName> [--registry-id N] (#11 step 11e): builds a B8G8R8A8 render pipeline
// from two functions of a SYSTEM metallib through the bundle. The bundle's own "spvcache hit ..." / "FALLBACK ..." lines go to
// stderr. Prints "syspipe: pipeline non-nil" or NIL with the NSError. Nothing is drawn.
static int cmd_syspipe(const char *path, const char *vn, const char *fn_, uint64_t rid, int haveRid) {
    id<MTLDevice> dev = pick_device(rid, haveRid); if (!dev) return 2;
    NSError *err = nil;
    id<MTLLibrary> lib = [dev newLibraryWithURL:[NSURL fileURLWithPath:@(path)] error:&err];
    printf("syspipe: library %s\n", path);
    if (err) perr("newLibraryWithURL", err);
    if (!lib) { printf("syspipe: FAIL library is nil\n"); return 2; }
    id<MTLFunction> vs = [lib newFunctionWithName:@(vn)], fs = [lib newFunctionWithName:@(fn_)];
    printf("syspipe: vertex %s=%s fragment %s=%s\n", vn, vs ? "non-nil" : "NIL", fn_, fs ? "non-nil" : "NIL");
    if (!vs || !fs) { printf("syspipe: FAIL function missing\n"); return 2; }
    MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
    pd.vertexFunction = vs; pd.fragmentFunction = fs; pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    err = nil; id<MTLRenderPipelineState> pso = [dev newRenderPipelineStateWithDescriptor:pd error:&err];
    if (err) perr("newRenderPipelineState", err);
    printf("syspipe: pipeline %s%s%s\n", pso ? "non-nil" : "NIL", pso ? " class " : "", pso ? class_getName(object_getClass(pso)) : "");
    printf(pso ? "syspipe: PASS\n" : "syspipe: FAIL\n");
    return pso ? 0 : 2;
}

// ---------------------------------------------------------------------------------------------------------------
// #11 step 11e oracles (m11e): quad / blend / compute / sysdraw. Every expected image is computed on the CPU below.
// ---------------------------------------------------------------------------------------------------------------
static const char *kQuadSrc =
"#include <metal_stdlib>\n"
"using namespace metal;\n"
"struct QIn { float2 pos [[attribute(0)]]; float2 uv [[attribute(1)]]; };\n"
"struct QOut { float4 pos [[position]]; float2 uv; };\n"
"vertex QOut quad_vs(QIn in [[stage_in]], constant float4 &xf [[buffer(1)]]) {\n"
"    QOut o; o.pos = float4(in.pos * xf.xy + xf.zw, 0.0, 1.0); o.uv = in.uv; return o; }\n"
"fragment float4 quad_fs(QOut in [[stage_in]], texture2d<float> tex [[texture(0)]], sampler smp [[sampler(0)]],\n"
"                        constant float4 &tint [[buffer(0)]]) { return tex.sample(smp, in.uv) * tint; }\n"
"vertex float4 blend_vs(uint vid [[vertex_id]], constant float4 &rect [[buffer(0)]]) {\n"
"    float2 c = float2(float(vid & 1u), float(vid >> 1));\n"
"    return float4(rect.xy + rect.zw * c, 0.0, 1.0); }\n"
"fragment float4 blend_fs(constant float4 &color [[buffer(0)]]) { return float4(color.rgb * color.a, color.a); }\n"
"fragment float4 fb_fs(float4 dst [[color(0)]], constant float4 &k [[buffer(0)]], constant float4 &c [[buffer(1)]]) { return dst * k + c; }\n"
"struct FbcOut { float4 pos [[position]]; float4 color; float2 texcoord0_0; };\n"
"vertex FbcOut fbcopy_vs(uint vid [[vertex_id]], constant float4 &rect [[buffer(0)]]) {\n"
"    float2 c = float2(float(vid & 1u), float(vid >> 1)); FbcOut o; o.pos = float4(rect.xy + rect.zw * c, 0.0, 1.0); o.color = float4(0.0);\n"
"    o.texcoord0_0 = float2((o.pos.x * 0.5 + 0.5) * 256.0, (0.5 - o.pos.y * 0.5) * 256.0); return o; }\n"
"vertex float4 fbmulti_vs(uint vid [[vertex_id]], constant float4 *rects [[buffer(0)]]) {\n"
"    uint q = vid / 6u; uint k = vid % 6u; uint idx = (k == 0u) ? 0u : (k == 1u) ? 1u : (k == 2u) ? 2u : (k == 3u) ? 2u : (k == 4u) ? 1u : 3u;\n"
"    float4 r = rects[q]; float2 c = float2(float(idx & 1u), float(idx >> 1)); return float4(r.xy + r.zw * c, 0.0, 1.0); }\n"
"kernel void fill_buf(device uint *out [[buffer(0)]], constant uint &n [[buffer(1)]], uint gid [[thread_position_in_grid]]) {\n"
"    if (gid < n) out[gid] = gid * 2654435761u ^ (gid >> 3); }\n"
"kernel void write_tex(texture2d<float, access::write> t [[texture(0)]], uint2 gid [[thread_position_in_grid]]) {\n"
"    if (gid.x < t.get_width() && gid.y < t.get_height())\n"
"        t.write(float4(float(gid.x) / 255.0, float(gid.y) / 255.0, float((gid.x * 3u + gid.y) & 255u) / 255.0, 1.0), gid); }\n";

#if defined(__arm64__)
#define N_MANAGED MTLStorageModeShared   /* Apple silicon has no Managed textures (host Mac sanity runs) */
#else
#define N_MANAGED MTLStorageModeManaged
#endif
static id<MTLLibrary> n_lib(id<MTLDevice> dev) {
    NSError *err = nil; id<MTLLibrary> lib = [dev newLibraryWithSource:@(kQuadSrc) options:nil error:&err];
    perr("newLibraryWithSource", err);
    printf("mtlprobe: step library (11e sources): %s\n", lib ? "non-nil" : "NIL");
    return lib;
}

// m11h9 (gaps2): sources for the texture oracles (A8, 3D, mip levels, IOSurface formats). Full-target quad from vertex_id + the quad's rect in buffer(0); uv.y = 0 at the TOP row.
static const char *kG2Src =
"#include <metal_stdlib>\n"
"using namespace metal;\n"
"struct G2O { float4 pos [[position]]; float2 uv; };\n"
"vertex G2O g2_vs(uint vid [[vertex_id]], constant float4 &rect [[buffer(0)]]) {\n"
"    float2 c = float2(float(vid & 1u), float(vid >> 1)); G2O o; o.pos = float4(rect.xy + rect.zw * c, 0.0, 1.0); o.uv = float2(c.x, 1.0 - c.y); return o; }\n"
"fragment float4 g2_tex2d(G2O in [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]]) { return t.sample(s, in.uv); }\n"
"fragment float4 g2_tex3d(G2O in [[stage_in]], texture3d<float> t [[texture(0)]], sampler s [[sampler(0)]], constant float &z [[buffer(0)]]) { return t.sample(s, float3(in.uv, z)); }\n"
"fragment float4 g2_lod(G2O in [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]], constant float &lod [[buffer(0)]]) { return t.sample(s, in.uv, level(lod)); }\n"
"fragment float4 g2_flat(G2O in [[stage_in]], constant float4 &c [[buffer(0)]]) { return c; }\n";
static id<MTLLibrary> g2_lib(id<MTLDevice> dev) {
    NSError *err = nil; id<MTLLibrary> lib = [dev newLibraryWithSource:@(kG2Src) options:nil error:&err];
    perr("newLibraryWithSource (g2)", err);
    return lib;
}

// bundle 9 (app crash study item 2): cube texture oracle.  A 64x64 BGRA8 cube with one colour per face (replaceRegion...slice:), sampled in the six axis directions into a 6x1 target.
// Metal's face order is +X, -X, +Y, -Y, +Z, -Z = slices 0..5; a direction along an axis hits the centre of its face, so the expected pixel k is face k's colour whatever the face orientation.
static const char *kCubeSrc =
"#include <metal_stdlib>\n"
"using namespace metal;\n"
"struct CubeO { float4 pos [[position]]; };\n"
"vertex CubeO cube_vs(uint vid [[vertex_id]]) { float2 c = float2(float(vid & 1u), float(vid >> 1)); CubeO o; o.pos = float4(c * 2.0 - 1.0, 0.0, 1.0); return o; }\n"
"fragment float4 cube_fs(CubeO in [[stage_in]], texturecube<float> t [[texture(0)]], sampler s [[sampler(0)]]) {\n"
"    uint i = uint(in.pos.x);\n"
"    float3 d = i == 0u ? float3(1.0, 0.0, 0.0) : i == 1u ? float3(-1.0, 0.0, 0.0) : i == 2u ? float3(0.0, 1.0, 0.0) : i == 3u ? float3(0.0, -1.0, 0.0) : i == 4u ? float3(0.0, 0.0, 1.0) : float3(0.0, 0.0, -1.0);\n"
"    return t.sample(s, d); }\n";
static id<MTLLibrary> cube_lib(id<MTLDevice> dev) {
    NSError *err = nil; id<MTLLibrary> lib = [dev newLibraryWithSource:@(kCubeSrc) options:nil error:&err];
    perr("newLibraryWithSource (cube)", err);
    return lib;
}
static int write_png_wh(const char *path, const uint8_t *bgra, int w, int h) {
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGDataProviderRef dp = CGDataProviderCreateWithData(NULL, bgra, (size_t)w * h * 4, NULL);
    CGImageRef img = CGImageCreate(w, h, 8, 32, (size_t)w * 4, cs, kCGBitmapByteOrder32Little | kCGImageAlphaNoneSkipFirst, dp, NULL, false, kCGRenderingIntentDefault);
    int ok = 0;
    if (img) {
        CGImageDestinationRef dst = CGImageDestinationCreateWithURL((__bridge CFURLRef)[NSURL fileURLWithPath:@(path)], CFSTR("public.png"), 1, NULL);
        if (dst) { CGImageDestinationAddImage(dst, img, NULL); ok = CGImageDestinationFinalize(dst); CFRelease(dst); }
        CGImageRelease(img);
    }
    CGDataProviderRelease(dp); CGColorSpaceRelease(cs);
    return ok;
}
static id<MTLTexture> n_tex(id<MTLDevice> dev, MTLPixelFormat pf, int w, int h, MTLTextureUsage u, MTLStorageMode sm) {
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:pf width:w height:h mipmapped:NO];
    td.usage = u; td.storageMode = sm;
    return [dev newTextureWithDescriptor:td];
}
static BOOL n_run(id<MTLCommandBuffer> cb, const char *tag) {
    [cb commit]; [cb waitUntilCompleted];
    printf("mtlprobe: %s command buffer status=%ld\n", tag, (long)cb.status);
    if (cb.error) perr(tag, cb.error);
    return cb.status == MTLCommandBufferStatusCompleted && !cb.error;
}
// Copies a texture into a fresh Shared buffer (one blit command buffer) and returns the bytes (nil on failure).
static NSData *n_readback(id<MTLDevice> dev, id<MTLCommandQueue> q, id<MTLTexture> t, int w, int h, int bpp) {
    id<MTLBuffer> buf = [dev newBufferWithLength:(NSUInteger)w * h * bpp options:MTLResourceStorageModeShared];
    if (!buf) return nil;
    memset(buf.contents, 0xEE, (size_t)w * h * bpp);
    id<MTLCommandBuffer> cb = [q commandBuffer];
    id<MTLBlitCommandEncoder> be = [cb blitCommandEncoder];
    [be copyFromTexture:t sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(w,h,1)
               toBuffer:buf destinationOffset:0 destinationBytesPerRow:(NSUInteger)w * bpp destinationBytesPerImage:(NSUInteger)w * h * bpp];
    [be endEncoding];
    if (!n_run(cb, "readback")) return nil;
    return [NSData dataWithBytes:buf.contents length:(NSUInteger)w * h * bpp];
}
// 64x64 RGBA8 source pattern: R = x*4, G = ((x^y)&63)*4, B = (y*4)&254, A = 255.
static void n_pattern(uint8_t *p) {
    for (int y = 0; y < 64; y++) for (int x = 0; x < 64; x++) {
        uint8_t *q = p + (y * 64 + x) * 4; q[0] = (uint8_t)(x * 4); q[1] = (uint8_t)(((x ^ y) & 63) * 4); q[2] = (uint8_t)((y * 4) & 254); q[3] = 255; }
}
static id<MTLTexture> n_source_texture(id<MTLDevice> dev, MTLPixelFormat pf, MTLStorageMode sm) {
    id<MTLTexture> t = n_tex(dev, pf, 64, 64, MTLTextureUsageShaderRead, sm);
    if (!t) return nil;
    uint8_t p[64 * 64 * 4]; n_pattern(p);
    if (pf == MTLPixelFormatBGRA8Unorm) for (int i = 0; i < 64 * 64; i++) { uint8_t r = p[i*4]; p[i*4] = p[i*4+2]; p[i*4+2] = r; }
    [t replaceRegion:MTLRegionMake2D(0, 0, 64, 64) mipmapLevel:0 withBytes:p bytesPerRow:64 * 4];
    return t;
}
static int n_report_diff(const char *tag, const uint8_t *got /*BGRA*/, const uint8_t *exp /*BGRA*/, int npix, int tol, const char *png, int w, int h) {
    int diff = 0, maxd = 0, firstBad = -1;
    for (int i = 0; i < npix; i++) {
        int m = 0; for (int c = 0; c < 4; c++) { int d = abs((int)got[i*4+c] - (int)exp[i*4+c]); if (d > m) m = d; }
        if (m > maxd) maxd = m;
        if (m > tol) { diff++; if (firstBad < 0) firstBad = i; }
    }
    if (png) { if (write_png_wh(png, got, w, h)) printf("mtlprobe: wrote %s\n", png); else printf("mtlprobe: WARN PNG write failed for %s\n", png); }
    if (firstBad >= 0) { int i = firstBad; printf("mtlprobe: first bad pixel (%d,%d): got BGRA %u,%u,%u,%u expected %u,%u,%u,%u\n", i % w, i / w,
        got[i*4], got[i*4+1], got[i*4+2], got[i*4+3], exp[i*4], exp[i*4+1], exp[i*4+2], exp[i*4+3]); }
    printf("mtlprobe: %s: %d of %d pixels differ (tolerance %d LSB); max per-channel difference %d\n", tag, diff, npix, tol, maxd);
    return diff;
}

// ---- (a) quad ----
static int cmd_quad(uint64_t rid, int haveRid, const char *out) {
    id<MTLDevice> dev = pick_device(rid, haveRid); if (!dev) return 2;
    id<MTLLibrary> lib = n_lib(dev); if (!lib) return 2;
    id<MTLFunction> vs = [lib newFunctionWithName:@"quad_vs"], fs = [lib newFunctionWithName:@"quad_fs"];
    if (!vs || !fs) { printf("mtlprobe: FAIL functions\n"); return 2; }
    MTLVertexDescriptor *vd = [MTLVertexDescriptor vertexDescriptor];
    vd.attributes[0].format = MTLVertexFormatFloat2; vd.attributes[0].offset = 0; vd.attributes[0].bufferIndex = 0;
    vd.attributes[1].format = MTLVertexFormatFloat2; vd.attributes[1].offset = 8; vd.attributes[1].bufferIndex = 0;
    vd.layouts[0].stride = 16; vd.layouts[0].stepFunction = MTLVertexStepFunctionPerVertex; vd.layouts[0].stepRate = 1;
    MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
    pd.vertexFunction = vs; pd.fragmentFunction = fs; pd.vertexDescriptor = vd; pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    NSError *err = nil; id<MTLRenderPipelineState> pso = [dev newRenderPipelineStateWithDescriptor:pd error:&err];
    perr("newRenderPipelineState", err);
    printf("mtlprobe: step pipeline: %s\n", pso ? "non-nil" : "NIL");
    if (!pso) { printf("mtlprobe: FAIL pipeline\n"); return 2; }
    MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new];
    sd.minFilter = MTLSamplerMinMagFilterNearest; sd.magFilter = MTLSamplerMinMagFilterNearest;
    sd.sAddressMode = MTLSamplerAddressModeClampToEdge; sd.tAddressMode = MTLSamplerAddressModeClampToEdge;
    id<MTLSamplerState> ss = [dev newSamplerStateWithDescriptor:sd];
    printf("mtlprobe: step sampler: %s\n", ss ? "non-nil" : "NIL");
    id<MTLTexture> src = n_source_texture(dev, MTLPixelFormatRGBA8Unorm, N_MANAGED);
    id<MTLTexture> tgt = n_tex(dev, MTLPixelFormatBGRA8Unorm, W, H, MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead, MTLStorageModePrivate);
    float verts[16] = { -1, 1, 0, 0,   1, 1, 1, 0,   -1, -1, 0, 1,   1, -1, 1, 1 };
    uint16_t idx[6] = { 0, 1, 2, 2, 1, 3 };
    id<MTLBuffer> vb = [dev newBufferWithLength:sizeof verts options:MTLResourceStorageModeShared];
    id<MTLBuffer> ib = [dev newBufferWithLength:sizeof idx options:MTLResourceStorageModeShared];
    if (!ss || !src || !tgt || !vb || !ib) { printf("mtlprobe: FAIL alloc\n"); return 2; }
    memcpy(vb.contents, verts, sizeof verts); memcpy(ib.contents, idx, sizeof idx);
    id<MTLCommandQueue> q = [dev newCommandQueue]; id<MTLCommandBuffer> cb = [q commandBuffer];
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = tgt; rp.colorAttachments[0].loadAction = MTLLoadActionClear; rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    rp.colorAttachments[0].clearColor = MTLClearColorMake(10.0 / 255, 20.0 / 255, 30.0 / 255, 1.0);
    id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:rp];
    float xf[4] = { 0.75f, 0.75f, 0, 0 }, tint[4] = { 1.0f, 0.75f, 0.5f, 1.0f };
    [re setRenderPipelineState:pso];
    [re setViewport:(MTLViewport){ 0, 0, W, H, 0, 1 }];
    [re setScissorRect:(MTLScissorRect){ 0, 0, W, H }];
    [re setVertexBuffer:vb offset:0 atIndex:0];
    [re setVertexBytes:xf length:sizeof xf atIndex:1];
    [re setFragmentBytes:tint length:sizeof tint atIndex:0];
    [re setFragmentTexture:src atIndex:0];
    [re setFragmentSamplerState:ss atIndex:0];
    [re drawIndexedPrimitives:MTLPrimitiveTypeTriangle indexCount:6 indexType:MTLIndexTypeUInt16 indexBuffer:ib indexBufferOffset:0];
    [re endEncoding];
    if (!n_run(cb, "quad")) { printf("mtlprobe: FAIL quad (command buffer)\n"); return 2; }
    NSData *got = n_readback(dev, q, tgt, W, H, 4); if (!got) { printf("mtlprobe: FAIL readback\n"); return 2; }
    // CPU reference (BGRA bytes).
    static uint8_t exp[W * H * 4]; uint8_t pat[64 * 64 * 4]; n_pattern(pat);
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        uint8_t *e = exp + (y * W + x) * 4;
        if (x >= 32 && x < 224 && y >= 32 && y < 224) {
            int tx = (int)floorf(((x + 0.5f - 32) / 192.0f) * 64.0f), ty = (int)floorf(((y + 0.5f - 32) / 192.0f) * 64.0f);
            const uint8_t *t = pat + (ty * 64 + tx) * 4;
            e[2] = (uint8_t)lrintf(t[0] / 255.0f * tint[0] * 255.0f); e[1] = (uint8_t)lrintf(t[1] / 255.0f * tint[1] * 255.0f);
            e[0] = (uint8_t)lrintf(t[2] / 255.0f * tint[2] * 255.0f); e[3] = (uint8_t)lrintf(t[3] / 255.0f * tint[3] * 255.0f);
        } else { e[0] = 30; e[1] = 20; e[2] = 10; e[3] = 255; }
    }
    int diff = n_report_diff("quad", got.bytes, exp, W * H, 0, out, W, H);
    printf(diff == 0 ? "mtlprobe: PASS quad (exact)\n" : "mtlprobe: FAIL quad\n");
    return diff == 0 ? 0 : 1;
}

// ---- (b) blend ----
static int cmd_blend(uint64_t rid, int haveRid, const char *out) {
    id<MTLDevice> dev = pick_device(rid, haveRid); if (!dev) return 2;
    id<MTLLibrary> lib = n_lib(dev); if (!lib) return 2;
    id<MTLFunction> vs = [lib newFunctionWithName:@"blend_vs"], fs = [lib newFunctionWithName:@"blend_fs"];
    if (!vs || !fs) { printf("mtlprobe: FAIL functions\n"); return 2; }
    MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
    pd.vertexFunction = vs; pd.fragmentFunction = fs;
    MTLRenderPipelineColorAttachmentDescriptor *ca = pd.colorAttachments[0];
    ca.pixelFormat = MTLPixelFormatBGRA8Unorm; ca.blendingEnabled = YES;
    ca.rgbBlendOperation = MTLBlendOperationAdd; ca.alphaBlendOperation = MTLBlendOperationAdd;
    ca.sourceRGBBlendFactor = MTLBlendFactorOne; ca.sourceAlphaBlendFactor = MTLBlendFactorOne;
    ca.destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha; ca.destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
    NSError *err = nil; id<MTLRenderPipelineState> pso = [dev newRenderPipelineStateWithDescriptor:pd error:&err];
    perr("newRenderPipelineState", err);
    printf("mtlprobe: step pipeline: %s\n", pso ? "non-nil" : "NIL");
    if (!pso) { printf("mtlprobe: FAIL pipeline\n"); return 2; }
    id<MTLTexture> tgt = n_tex(dev, MTLPixelFormatBGRA8Unorm, W, H, MTLTextureUsageRenderTarget, MTLStorageModePrivate);
    if (!tgt) { printf("mtlprobe: FAIL alloc\n"); return 2; }
    struct { int x0, y0, x1, y1; float c[4]; } quads[2] = { { 40, 40, 168, 168, { 0.8f, 0.2f, 0.1f, 0.5f } }, { 88, 88, 216, 216, { 0.1f, 0.6f, 0.9f, 0.25f } } };
    id<MTLCommandQueue> q = [dev newCommandQueue]; id<MTLCommandBuffer> cb = [q commandBuffer];
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = tgt; rp.colorAttachments[0].loadAction = MTLLoadActionClear; rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    rp.colorAttachments[0].clearColor = MTLClearColorMake(20.0 / 255, 40.0 / 255, 60.0 / 255, 1.0);
    id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:rp];
    [re setRenderPipelineState:pso];
    for (int k = 0; k < 2; k++) {
        float rect[4] = { quads[k].x0 / 128.0f - 1.0f, 1.0f - quads[k].y1 / 128.0f, (quads[k].x1 - quads[k].x0) / 128.0f, (quads[k].y1 - quads[k].y0) / 128.0f };
        [re setVertexBytes:rect length:sizeof rect atIndex:0];
        [re setFragmentBytes:quads[k].c length:sizeof quads[k].c atIndex:0];
        [re drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
    }
    [re endEncoding];
    if (!n_run(cb, "blend")) { printf("mtlprobe: FAIL blend (command buffer)\n"); return 2; }
    NSData *got = n_readback(dev, q, tgt, W, H, 4); if (!got) { printf("mtlprobe: FAIL readback\n"); return 2; }
    static uint8_t exp[W * H * 4];
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        float d[4] = { 60.0f / 255, 40.0f / 255, 20.0f / 255, 1.0f };   // B,G,R,A as float; clear
        // store between draws is unorm8: quantise after each quad
        uint8_t q8[4] = { 60, 40, 20, 255 };
        for (int k = 0; k < 2; k++) {
            if (x >= quads[k].x0 && x < quads[k].x1 && y >= quads[k].y0 && y < quads[k].y1) {
                const float *c = quads[k].c; float a = c[3];
                float s[4] = { c[2] * a, c[1] * a, c[0] * a, a };   // B,G,R,A premultiplied
                for (int m = 0; m < 4; m++) d[m] = s[m] + (q8[m] / 255.0f) * (1.0f - a);
                for (int m = 0; m < 4; m++) { float v = d[m] < 0 ? 0 : d[m] > 1 ? 1 : d[m]; q8[m] = (uint8_t)lrintf(v * 255.0f); }
            }
        }
        memcpy(exp + (y * W + x) * 4, q8, 4);
    }
    int d1 = n_report_diff("blend (tolerance 1 LSB)", got.bytes, exp, W * H, 1, out, W, H);
    int d0 = n_report_diff("blend (tolerance 0, informational)", got.bytes, exp, W * H, 0, NULL, W, H);
    printf("mtlprobe: blend: %d pixels exact-different, %d beyond 1 LSB\n", d0, d1);
    printf(d1 == 0 ? (d0 == 0 ? "mtlprobe: PASS blend (exact)\n" : "mtlprobe: PASS blend (within 1 LSB)\n") : "mtlprobe: FAIL blend\n");
    return d1 == 0 ? 0 : 1;
}

// ---- (c) compute ----
static int cmd_compute(uint64_t rid, int haveRid, const char *out) {
    id<MTLDevice> dev = pick_device(rid, haveRid); if (!dev) return 2;
    id<MTLLibrary> lib = n_lib(dev); if (!lib) return 2;
    NSError *err = nil;
    id<MTLFunction> fa = [lib newFunctionWithName:@"fill_buf"], fb = [lib newFunctionWithName:@"write_tex"];
    if (!fa || !fb) { printf("mtlprobe: FAIL functions\n"); return 2; }
    id<MTLComputePipelineState> pa = [dev newComputePipelineStateWithFunction:fa error:&err]; perr("pipeline fill_buf", err);
    id<MTLComputePipelineState> pb = [dev newComputePipelineStateWithFunction:fb error:&err]; perr("pipeline write_tex", err);
    printf("mtlprobe: step compute pipelines: fill_buf=%s write_tex=%s\n", pa ? "non-nil" : "NIL", pb ? "non-nil" : "NIL");
    if (!pa || !pb) { printf("mtlprobe: FAIL compute pipeline\n"); return 2; }
    id<MTLBuffer> b1 = [dev newBufferWithLength:4096 options:MTLResourceStorageModeShared], b2 = [dev newBufferWithLength:4096 options:MTLResourceStorageModeShared];
    id<MTLTexture> t1 = n_tex(dev, MTLPixelFormatRGBA8Unorm, 64, 64, MTLTextureUsageShaderWrite | MTLTextureUsageShaderRead, MTLStorageModePrivate);
    id<MTLTexture> t2 = n_tex(dev, MTLPixelFormatRGBA8Unorm, 60, 60, MTLTextureUsageShaderWrite | MTLTextureUsageShaderRead, MTLStorageModePrivate);
    if (!b1 || !b2 || !t1 || !t2) { printf("mtlprobe: FAIL alloc\n"); return 2; }
    memset(b1.contents, 0xAA, 4096); memset(b2.contents, 0xAA, 4096);
    id<MTLCommandQueue> q = [dev newCommandQueue]; id<MTLCommandBuffer> cb = [q commandBuffer];
    id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
    if (!ce) { printf("mtlprobe: FAIL compute encoder\n"); return 2; }
    uint32_t n = 1000;
    [ce setComputePipelineState:pa];
    [ce setBuffer:b1 offset:0 atIndex:0]; [ce setBytes:&n length:4 atIndex:1];
    [ce dispatchThreads:MTLSizeMake(1000, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
    [ce setBuffer:b2 offset:0 atIndex:0];
    [ce dispatchThreadgroups:MTLSizeMake(16, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
    [ce setComputePipelineState:pb];
    [ce setTexture:t1 atIndex:0];
    [ce dispatchThreadgroups:MTLSizeMake(8, 8, 1) threadsPerThreadgroup:MTLSizeMake(8, 8, 1)];
    [ce setTexture:t2 atIndex:0];
    [ce dispatchThreads:MTLSizeMake(60, 60, 1) threadsPerThreadgroup:MTLSizeMake(8, 8, 1)];
    [ce endEncoding];
    if (!n_run(cb, "compute")) { printf("mtlprobe: FAIL compute (command buffer)\n"); return 2; }
    int bad = 0; const uint32_t *o1 = b1.contents, *o2 = b2.contents;
    for (uint32_t i = 0; i < 1024; i++) {
        uint32_t e = i < n ? (i * 2654435761u ^ (i >> 3)) : 0xAAAAAAAAu;
        if (o1[i] != e) { if (bad < 4) printf("mtlprobe: dispatchThreads buf[%u] = 0x%08x expected 0x%08x\n", i, o1[i], e); bad++; }
        if (o2[i] != e) { if (bad < 4) printf("mtlprobe: dispatchThreadgroups buf[%u] = 0x%08x expected 0x%08x\n", i, o2[i], e); bad++; }
    }
    printf("mtlprobe: compute buffer results: %d mismatching words of 2048 (dispatchThreads 1000 in 64s = 15 full + tail 40; dispatchThreadgroups 16 x 64 with guard)\n", bad);
    int tb = 0;
    id<MTLTexture> ts[2] = { t1, t2 }; int dims[2] = { 64, 60 };
    static uint8_t big[W * H * 4]; memset(big, 0, sizeof big);
    for (int k = 0; k < 2; k++) {
        int w = dims[k]; NSData *g = n_readback(dev, q, ts[k], w, w, 4); if (!g) { printf("mtlprobe: FAIL readback\n"); return 2; }
        const uint8_t *p = g.bytes; int bad2 = 0;
        for (int y = 0; y < w; y++) for (int x = 0; x < w; x++) {
            uint8_t e[4] = { (uint8_t)x, (uint8_t)y, (uint8_t)((x * 3 + y) & 255), 255 };   // RGBA
            const uint8_t *r = p + (y * w + x) * 4;
            if (memcmp(r, e, 4)) { if (bad2 < 3) printf("mtlprobe: texture %d (%d,%d) = %u,%u,%u,%u expected %u,%u,%u,%u\n", k, x, y, r[0], r[1], r[2], r[3], e[0], e[1], e[2], e[3]); bad2++; }
            // PNG montage: t1 at (0,0), t2 at (128,0), shown 2x
            for (int dy = 0; dy < 2; dy++) for (int dx = 0; dx < 2; dx++) { uint8_t *bb = big + (((y * 2 + dy) * W) + (k * 128 + x * 2 + dx)) * 4; bb[0] = r[2]; bb[1] = r[1]; bb[2] = r[0]; bb[3] = 255; }
        }
        printf("mtlprobe: compute texture %d (%dx%d, %s): %d mismatching pixels\n", k, w, w, k ? "dispatchThreads 60x60 tpt 8x8" : "dispatchThreadgroups 8x8 of 8x8", bad2);
        tb += bad2;
    }
    if (out) { if (write_png_wh(out, big, W, H)) printf("mtlprobe: wrote %s\n", out); }
    printf("mtlprobe: compute: %d mismatches total\n", bad + tb);
    printf(bad + tb == 0 ? "mtlprobe: PASS compute (0 mismatches)\n" : "mtlprobe: FAIL compute\n");
    return bad + tb == 0 ? 0 : 1;
}

// ---- (d) sysdraw: CoreDisplay ViewportToNDC + TextureCopy ----
// ViewportToNDC (AIR): buffer(0) packed_float2 vertexArray[vid] (device), buffer(1) packed_float2 texCoords[vid] (device),
// buffer(2) float4x4 mvpMatrix (device), vertex_id. Out: position, user(texturecoord) float2. TextureCopy: texture(0), constexpr
// sampler {linear, clamp_to_edge, coord::pixel}, returns tex.sample(sampler, texCoord).
static int cmd_sysdraw(uint64_t rid, int haveRid, const char *out) {
    id<MTLDevice> dev = pick_device(rid, haveRid); if (!dev) return 2;
    NSError *err = nil;
    id<MTLLibrary> lib = [dev newLibraryWithURL:[NSURL fileURLWithPath:@"/System/Library/Frameworks/CoreDisplay.framework/Versions/A/Resources/default.metallib"] error:&err];
    perr("newLibraryWithURL", err);
    id<MTLFunction> vs = lib ? [lib newFunctionWithName:@"ViewportToNDC"] : nil, fs = lib ? [lib newFunctionWithName:@"TextureCopy"] : nil;
    if (!vs || !fs) { printf("mtlprobe: FAIL CoreDisplay functions\n"); return 2; }
    MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
    pd.vertexFunction = vs; pd.fragmentFunction = fs; pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    id<MTLRenderPipelineState> pso = [dev newRenderPipelineStateWithDescriptor:pd error:&err]; perr("newRenderPipelineState", err);
    printf("mtlprobe: step pipeline: %s\n", pso ? "non-nil" : "NIL");
    if (!pso) { printf("mtlprobe: FAIL pipeline\n"); return 2; }
    id<MTLTexture> src = n_source_texture(dev, MTLPixelFormatBGRA8Unorm, N_MANAGED);
    id<MTLTexture> tgt = n_tex(dev, MTLPixelFormatBGRA8Unorm, 64, 64, MTLTextureUsageRenderTarget, MTLStorageModePrivate);
    id<MTLBuffer> b0 = [dev newBufferWithLength:256 options:MTLResourceStorageModeShared], b1 = [dev newBufferWithLength:256 options:MTLResourceStorageModeShared];
    if (!src || !tgt || !b0 || !b1) { printf("mtlprobe: FAIL alloc\n"); return 2; }
    float pos[8] = { -1, 1,  1, 1,  -1, -1,  1, -1 };          // strip: TL TR BL BR
    float tc[8]  = { 0, 0,  64, 0,  0, 64,  64, 64 };          // pixel coordinates
    memset(b0.contents, 0x7F, 256); memset(b1.contents, 0x7F, 256);
    memcpy((uint8_t *)b0.contents + 16, pos, sizeof pos);      // nonzero offsets exercise setVertexBuffer:offset:
    memcpy((uint8_t *)b1.contents + 32, tc, sizeof tc);
    float mvp[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
    id<MTLCommandQueue> q = [dev newCommandQueue]; id<MTLCommandBuffer> cb = [q commandBuffer];
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = tgt; rp.colorAttachments[0].loadAction = MTLLoadActionClear; rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    rp.colorAttachments[0].clearColor = MTLClearColorMake(1, 0, 1, 1);
    id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:rp];
    [re setRenderPipelineState:pso];
    [re setVertexBuffer:b0 offset:16 atIndex:0];
    [re setVertexBuffer:b1 offset:32 atIndex:1];
    [re setVertexBytes:mvp length:sizeof mvp atIndex:2];
    [re setFragmentTexture:src atIndex:0];
    [re drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
    [re endEncoding];
    if (!n_run(cb, "sysdraw")) { printf("mtlprobe: FAIL sysdraw (command buffer)\n"); return 2; }
    NSData *got = n_readback(dev, q, tgt, 64, 64, 4); if (!got) { printf("mtlprobe: FAIL readback\n"); return 2; }
    uint8_t pat[64 * 64 * 4]; n_pattern(pat);
    static uint8_t exp[64 * 64 * 4];
    for (int i = 0; i < 64 * 64; i++) { exp[i*4] = pat[i*4+2]; exp[i*4+1] = pat[i*4+1]; exp[i*4+2] = pat[i*4]; exp[i*4+3] = pat[i*4+3]; }   // source is BGRA-swapped pattern
    int diff = n_report_diff("sysdraw CoreDisplay TextureCopy", got.bytes, exp, 64 * 64, 0, out, 64, 64);
    printf(diff == 0 ? "mtlprobe: PASS sysdraw (exact)\n" : "mtlprobe: FAIL sysdraw\n");
    return diff == 0 ? 0 : 1;
}

// mtlprobe cube [--registry-id N]: bundle 9.  Create, upload per face, read back per face, sample in six directions (6 pixels, exact).
static const uint8_t cube_col[6][4] = { { 15, 220, 30, 255 }, { 60, 190, 70, 255 }, { 105, 160, 110, 255 }, { 150, 130, 150, 255 }, { 195, 100, 190, 255 }, { 240, 70, 230, 255 } };   // B, G, R, A per face
static int cmd_cube(uint64_t rid, int haveRid, const char *out) {
    id<MTLDevice> dev = pick_device(rid, haveRid); if (!dev) return 2;
    int fails = 0;
    #define CUBE_CHECK(tag, cond, ...) do { int ok_ = (cond) ? 1 : 0; printf("mtlprobe: cube %s: %s: ", ok_ ? "ok  " : "FAIL", tag); printf(__VA_ARGS__); printf("\n"); if (!ok_) fails++; } while (0)
    MTLTextureDescriptor *td = [MTLTextureDescriptor textureCubeDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm size:64 mipmapped:NO];
    td.usage = MTLTextureUsageShaderRead; td.storageMode = N_MANAGED;
    id<MTLTexture> cube = [dev newTextureWithDescriptor:td];
    CUBE_CHECK("create 64x64 BGRA8 cube", cube != nil, "type %lu", td.textureType);
    if (!cube) { printf("mtlprobe: FAIL cube (no texture; VectorKit aborts on this)\n"); return 1; }
    CUBE_CHECK("properties", cube.textureType == MTLTextureTypeCube && cube.width == 64 && cube.height == 64 && cube.arrayLength == 1 && cube.depth == 1 && cube.pixelFormat == MTLPixelFormatBGRA8Unorm && cube.mipmapLevelCount == 1,
               "type %lu %lux%lu array %lu depth %lu mips %lu", (unsigned long)cube.textureType, (unsigned long)cube.width, (unsigned long)cube.height, (unsigned long)cube.arrayLength, (unsigned long)cube.depth, (unsigned long)cube.mipmapLevelCount);
    static uint8_t face[64 * 64 * 4], back[64 * 64 * 4];
    for (int k = 0; k < 6; k++) { for (int i = 0; i < 64 * 64; i++) memcpy(face + i * 4, cube_col[k], 4);
        [cube replaceRegion:MTLRegionMake2D(0, 0, 64, 64) mipmapLevel:0 slice:k withBytes:face bytesPerRow:64 * 4 bytesPerImage:64 * 64 * 4]; }
    int badFaces = 0;
    for (int k = 0; k < 6; k++) { memset(back, 0, sizeof back); [cube getBytes:back bytesPerRow:64 * 4 bytesPerImage:64 * 64 * 4 fromRegion:MTLRegionMake2D(0, 0, 64, 64) mipmapLevel:0 slice:k];
        int bad = 0; for (int i = 0; i < 64 * 64; i++) if (memcmp(back + i * 4, cube_col[k], 4)) { bad = 1; break; } badFaces += bad; }
    CUBE_CHECK("per-face replaceRegion / getBytes", badFaces == 0, "%d of 6 faces differ (each face holds its own colour)", badFaces);
    id<MTLLibrary> lib = cube_lib(dev); id<MTLFunction> vs = lib ? [lib newFunctionWithName:@"cube_vs"] : nil, fs = lib ? [lib newFunctionWithName:@"cube_fs"] : nil;
    CUBE_CHECK("library + functions", vs && fs, "vs %s fs %s", vs ? "ok" : "NIL", fs ? "ok" : "NIL");
    if (!vs || !fs) { printf("mtlprobe: FAIL cube\n"); return 2; }
    MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new]; pd.vertexFunction = vs; pd.fragmentFunction = fs; pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    NSError *err = nil; id<MTLRenderPipelineState> pso = [dev newRenderPipelineStateWithDescriptor:pd error:&err]; perr("newRenderPipelineState (cube)", err);
    CUBE_CHECK("pipeline", pso != nil, "%s", pso ? "non-nil" : "NIL");
    MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new]; sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterNearest; sd.sAddressMode = sd.tAddressMode = sd.rAddressMode = MTLSamplerAddressModeClampToEdge;
    id<MTLSamplerState> ss = [dev newSamplerStateWithDescriptor:sd];
    id<MTLTexture> tgt = n_tex(dev, MTLPixelFormatBGRA8Unorm, 6, 1, MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead, MTLStorageModePrivate);
    if (!pso || !ss || !tgt) { printf("mtlprobe: FAIL cube (alloc)\n"); return 2; }
    id<MTLCommandQueue> q = [dev newCommandQueue]; id<MTLCommandBuffer> cb = [q commandBuffer];
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = tgt; rp.colorAttachments[0].loadAction = MTLLoadActionClear; rp.colorAttachments[0].storeAction = MTLStoreActionStore; rp.colorAttachments[0].clearColor = MTLClearColorMake(1, 0, 1, 1);
    id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:rp];
    [re setRenderPipelineState:pso]; [re setViewport:(MTLViewport){ 0, 0, 6, 1, 0, 1 }]; [re setScissorRect:(MTLScissorRect){ 0, 0, 6, 1 }];
    [re setFragmentTexture:cube atIndex:0]; [re setFragmentSamplerState:ss atIndex:0];
    [re drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4]; [re endEncoding];
    if (!n_run(cb, "cube")) { printf("mtlprobe: FAIL cube (command buffer)\n"); return 2; }
    NSData *got = n_readback(dev, q, tgt, 6, 1, 4); if (!got) { printf("mtlprobe: FAIL cube (readback)\n"); return 2; }
    static uint8_t exp[6 * 4]; for (int k = 0; k < 6; k++) memcpy(exp + k * 4, cube_col[k], 4);
    int diff = 0; const uint8_t *g = got.bytes; const char *dn[6] = { "+X", "-X", "+Y", "-Y", "+Z", "-Z" };
    for (int k = 0; k < 6; k++) { int ok = !memcmp(g + k * 4, exp + k * 4, 4); diff += !ok;
        printf("mtlprobe: cube sample %s: got B%u G%u R%u A%u expected B%u G%u R%u A%u %s\n", dn[k], g[k*4], g[k*4+1], g[k*4+2], g[k*4+3], exp[k*4], exp[k*4+1], exp[k*4+2], exp[k*4+3], ok ? "ok" : "MISMATCH"); }
    CUBE_CHECK("six axis-direction samples", diff == 0, "%d of 6 pixels differ", diff);
    (void)out;
    printf(fails == 0 ? "mtlprobe: PASS cube (6 of 6 pixels exact)\n" : "mtlprobe: FAIL cube (%d)\n", fails);
    return fails ? 1 : 0;
}


// bundle 10 (depth/stencil and MSAA): oracles for `mtlprobe depth` and `mtlprobe msaa`.  Both draw triangles from vertex_id + setVertexBytes: (ox, oy, sx, sy) place the unit right triangle
// (0,0) (1,0) (0,1) of "uv" space (u right, v UP, so row = (1 - v) * 16 in the 16x16 target) at uv = (ox,oy) + c * (sx,sy), and z is the clip depth.  Every expectation was checked on an Apple-silicon Mac first.
static const char *kDepthSrc =
"#include <metal_stdlib>\n"
"using namespace metal;\n"
"struct DepO { float4 pos [[position]]; };\n"
"vertex DepO dep_vs(uint vid [[vertex_id]], constant float4 &r [[buffer(0)]], constant float &z [[buffer(1)]]) {\n"
"    float2 c = vid == 0u ? float2(0.0, 0.0) : vid == 1u ? float2(1.0, 0.0) : float2(0.0, 1.0);\n"
"    DepO o; o.pos = float4((r.xy + c * r.zw) * 2.0 - 1.0, z, 1.0); return o; }\n"
"fragment float4 dep_fs(DepO in [[stage_in]], constant float4 &col [[buffer(0)]]) { return col; }\n";
static id<MTLLibrary> dep_lib(id<MTLDevice> dev) {
    NSError *err = nil; id<MTLLibrary> lib = [dev newLibraryWithSource:@(kDepthSrc) options:nil error:&err];
    perr("newLibraryWithSource (depth)", err);
    return lib;
}
static void dep_tri(id<MTLRenderCommandEncoder> re, float ox, float oy, float sx, float sy, float z, const float col[4]) {
    float r[4] = { ox, oy, sx, sy }; [re setVertexBytes:r length:sizeof r atIndex:0]; [re setVertexBytes:&z length:sizeof z atIndex:1]; [re setFragmentBytes:col length:16 atIndex:0];
    [re drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
}
static id<MTLDepthStencilState> dep_dss(id<MTLDevice> dev, MTLCompareFunction cmp, BOOL write, MTLCompareFunction scmp, MTLStencilOperation pass) {
    MTLDepthStencilDescriptor *d = [MTLDepthStencilDescriptor new]; d.depthCompareFunction = cmp; d.depthWriteEnabled = write;
    if (scmp != MTLCompareFunctionAlways || pass != MTLStencilOperationKeep) { MTLStencilDescriptor *sd = [MTLStencilDescriptor new]; sd.stencilCompareFunction = scmp; sd.depthStencilPassOperation = pass; sd.readMask = 0xFF; sd.writeMask = 0xFF; d.frontFaceStencil = sd; d.backFaceStencil = sd; }
    return [dev newDepthStencilStateWithDescriptor:d];
}
// the BGRA8 pixel (B, G, R, A) at uv (u, v) of a 16x16 readback
static const uint8_t *dep_px(NSData *g, float u, float v) { int x = (int)(u * 16.0f), y = 15 - (int)(v * 16.0f); return (const uint8_t *)g.bytes + (y * 16 + x) * 4; }
static int cmd_depth(uint64_t rid, int haveRid) {
    id<MTLDevice> dev = pick_device(rid, haveRid); if (!dev) return 2;
    int fails = 0;
    #define DEP_CHECK(tag, cond, ...) do { int ok_ = (cond) ? 1 : 0; printf("mtlprobe: depth %s: %s: ", ok_ ? "ok  " : "FAIL", tag); printf(__VA_ARGS__); printf("\n"); if (!ok_) fails++; } while (0)
    printf("mtlprobe: depth note: isDepth24Stencil8PixelFormatSupported = %d (the bundle must answer 0), supportsTextureSampleCount 1/2/4/8 = %d/%d/%d/%d\n", (int)dev.isDepth24Stencil8PixelFormatSupported,
           (int)[dev supportsTextureSampleCount:1], (int)[dev supportsTextureSampleCount:2], (int)[dev supportsTextureSampleCount:4], (int)[dev supportsTextureSampleCount:8]);
    // formats: Depth16Unorm, Depth32Float, Stencil8, Depth32Float_Stencil8 as Private RenderTarget|ShaderRead 2D textures
    static const MTLPixelFormat pfs[4] = { MTLPixelFormatDepth16Unorm, MTLPixelFormatDepth32Float, MTLPixelFormatStencil8, MTLPixelFormatDepth32Float_Stencil8 }; static const char *pfn[4] = { "Depth16Unorm", "Depth32Float", "Stencil8", "Depth32Float_Stencil8" };
    for (int k = 0; k < 4; k++) { id<MTLTexture> t = n_tex(dev, pfs[k], 32, 32, MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead, MTLStorageModePrivate);
        DEP_CHECK("create", t != nil && t.pixelFormat == pfs[k] && t.width == 32 && t.textureType == MTLTextureType2D && t.sampleCount == 1, "%s Private RenderTarget|ShaderRead 32x32: %s", pfn[k], t ? "ok" : "NIL"); }
    id<MTLTexture> shared = n_tex(dev, MTLPixelFormatDepth32Float, 32, 32, MTLTextureUsageRenderTarget, MTLStorageModeShared);
    printf("mtlprobe: depth note: Depth32Float with Shared storage -> %s (the bundle refuses it; an Apple GPU may accept it)\n", shared ? "texture" : "nil");
    id<MTLLibrary> lib = dep_lib(dev); id<MTLFunction> vs = lib ? [lib newFunctionWithName:@"dep_vs"] : nil, fs = lib ? [lib newFunctionWithName:@"dep_fs"] : nil;
    DEP_CHECK("library + functions", vs && fs, "vs %s fs %s", vs ? "ok" : "NIL", fs ? "ok" : "NIL");
    if (!vs || !fs) { printf("mtlprobe: FAIL depth\n"); return 2; }
    id<MTLCommandQueue> q = [dev newCommandQueue];
    const float RED[4] = { 1, 0, 0, 1 }, GREEN[4] = { 0, 1, 0, 1 };
    // ---- part 1: two overlapping triangles, depth test LESS.  A (far, z 0.7, red): uv (0,0) (1,0) (0,1).  B (near, z 0.3, green): the same triangle shifted to (0.3, 0.3).  Drawn in BOTH orders. ----
    MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new]; pd.vertexFunction = vs; pd.fragmentFunction = fs; pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm; pd.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
    NSError *err = nil; id<MTLRenderPipelineState> pso = [dev newRenderPipelineStateWithDescriptor:pd error:&err]; perr("newRenderPipelineState (depth)", err);
    DEP_CHECK("pipeline with depthAttachmentPixelFormat = Depth32Float", pso != nil, "%s", pso ? "non-nil" : "NIL");
    id<MTLDepthStencilState> less = dep_dss(dev, MTLCompareFunctionLess, YES, MTLCompareFunctionAlways, MTLStencilOperationKeep);
    if (!pso || !less) { printf("mtlprobe: FAIL depth (no pipeline / state)\n"); return 2; }
    for (int order = 0; order < 2; order++) {
        id<MTLTexture> tgt = n_tex(dev, MTLPixelFormatBGRA8Unorm, 16, 16, MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead, MTLStorageModePrivate), dtex = n_tex(dev, MTLPixelFormatDepth32Float, 16, 16, MTLTextureUsageRenderTarget, MTLStorageModePrivate);
        if (!tgt || !dtex) { printf("mtlprobe: FAIL depth (targets)\n"); return 2; }
        id<MTLCommandBuffer> cb = [q commandBuffer]; MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = tgt; rp.colorAttachments[0].loadAction = MTLLoadActionClear; rp.colorAttachments[0].storeAction = MTLStoreActionStore; rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
        rp.depthAttachment.texture = dtex; rp.depthAttachment.loadAction = MTLLoadActionClear; rp.depthAttachment.storeAction = MTLStoreActionStore; rp.depthAttachment.clearDepth = 1.0;
        id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:rp]; [re setRenderPipelineState:pso]; [re setDepthStencilState:less]; [re setViewport:(MTLViewport){ 0, 0, 16, 16, 0, 1 }];
        if (order == 0) { dep_tri(re, 0, 0, 1, 1, 0.7f, RED); dep_tri(re, 0.3f, 0.3f, 1, 1, 0.3f, GREEN); } else { dep_tri(re, 0.3f, 0.3f, 1, 1, 0.3f, GREEN); dep_tri(re, 0, 0, 1, 1, 0.7f, RED); }
        [re endEncoding];
        if (!n_run(cb, "depth")) { printf("mtlprobe: FAIL depth (command buffer)\n"); return 2; }
        NSData *g = n_readback(dev, q, tgt, 16, 16, 4); if (!g) { printf("mtlprobe: FAIL depth (readback)\n"); return 2; }
        const uint8_t *ov = dep_px(g, 0.40f, 0.40f), *ra = dep_px(g, 0.10f, 0.10f), *gb = dep_px(g, 0.80f, 0.70f), *bg = dep_px(g, 0.92f, 0.92f);   // overlap, A only, B only, neither
        printf("mtlprobe: depth order %s: overlap B%u G%u R%u, farther-only B%u G%u R%u, nearer-only B%u G%u R%u, outside B%u G%u R%u\n", order ? "near-then-far" : "far-then-near", ov[0], ov[1], ov[2], ra[0], ra[1], ra[2], gb[0], gb[1], gb[2], bg[0], bg[1], bg[2]);
        DEP_CHECK(order ? "overlap (near drawn first)" : "overlap (far drawn first)", ov[1] == 255 && ov[2] == 0 && ov[0] == 0, "the NEARER green wins at the overlap pixel whatever the draw order");
        DEP_CHECK(order ? "farther only (near drawn first)" : "farther only (far drawn first)", ra[2] == 255 && ra[1] == 0, "the farther red shows where the nearer triangle is not");
        DEP_CHECK(order ? "nearer only (near drawn first)" : "nearer only (far drawn first)", gb[1] == 255 && gb[2] == 0, "the nearer green shows outside the farther triangle");
        DEP_CHECK(order ? "outside both (near drawn first)" : "outside both (far drawn first)", bg[0] == 0 && bg[1] == 0 && bg[2] == 0, "the clear colour remains outside both");
    }
    // ---- part 1b: depth write OFF - the first (near) triangle writes no depth, so the later (far) one is not rejected by it ----
    { id<MTLDepthStencilState> nw = dep_dss(dev, MTLCompareFunctionLess, NO, MTLCompareFunctionAlways, MTLStencilOperationKeep);
      id<MTLTexture> tgt = n_tex(dev, MTLPixelFormatBGRA8Unorm, 16, 16, MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead, MTLStorageModePrivate), dtex = n_tex(dev, MTLPixelFormatDepth32Float, 16, 16, MTLTextureUsageRenderTarget, MTLStorageModePrivate);
      id<MTLCommandBuffer> cb = [q commandBuffer]; MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
      rp.colorAttachments[0].texture = tgt; rp.colorAttachments[0].loadAction = MTLLoadActionClear; rp.colorAttachments[0].storeAction = MTLStoreActionStore; rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
      rp.depthAttachment.texture = dtex; rp.depthAttachment.loadAction = MTLLoadActionClear; rp.depthAttachment.storeAction = MTLStoreActionStore; rp.depthAttachment.clearDepth = 1.0;
      id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:rp]; [re setRenderPipelineState:pso]; [re setDepthStencilState:nw]; [re setViewport:(MTLViewport){ 0, 0, 16, 16, 0, 1 }];
      dep_tri(re, 0.3f, 0.3f, 1, 1, 0.3f, GREEN); dep_tri(re, 0, 0, 1, 1, 0.7f, RED); [re endEncoding];
      if (!n_run(cb, "depth-nowrite")) { printf("mtlprobe: FAIL depth (command buffer, write off)\n"); return 2; }
      NSData *g = n_readback(dev, q, tgt, 16, 16, 4); if (!g) { printf("mtlprobe: FAIL depth (readback)\n"); return 2; }
      const uint8_t *ov = dep_px(g, 0.40f, 0.40f); DEP_CHECK("depth write off", ov[2] == 255 && ov[1] == 0, "with depth writes off the later farther red is not rejected (overlap B%u G%u R%u, expected red)", ov[0], ov[1], ov[2]); }
    // ---- part 2: stencil on Depth32Float_Stencil8.  A writes stencil 1 (compare Always, pass Replace, reference 1; depth write off); a full-screen-ish green triangle draws only where stencil == 1. ----
    { MTLRenderPipelineDescriptor *p2 = [MTLRenderPipelineDescriptor new]; p2.vertexFunction = vs; p2.fragmentFunction = fs; p2.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
      p2.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float_Stencil8; p2.stencilAttachmentPixelFormat = MTLPixelFormatDepth32Float_Stencil8;
      NSError *e2 = nil; id<MTLRenderPipelineState> ps = [dev newRenderPipelineStateWithDescriptor:p2 error:&e2]; perr("newRenderPipelineState (stencil)", e2);
      id<MTLDepthStencilState> wr = dep_dss(dev, MTLCompareFunctionAlways, NO, MTLCompareFunctionAlways, MTLStencilOperationReplace), eq = dep_dss(dev, MTLCompareFunctionAlways, NO, MTLCompareFunctionEqual, MTLStencilOperationKeep);
      DEP_CHECK("stencil pipeline (Depth32Float_Stencil8 for depth and stencil)", ps != nil && wr && eq, "%s", ps ? "non-nil" : "NIL");
      id<MTLTexture> tgt = n_tex(dev, MTLPixelFormatBGRA8Unorm, 16, 16, MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead, MTLStorageModePrivate), dst = n_tex(dev, MTLPixelFormatDepth32Float_Stencil8, 16, 16, MTLTextureUsageRenderTarget, MTLStorageModePrivate);
      if (ps && wr && eq && tgt && dst) {
        id<MTLCommandBuffer> cb = [q commandBuffer]; MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = tgt; rp.colorAttachments[0].loadAction = MTLLoadActionClear; rp.colorAttachments[0].storeAction = MTLStoreActionStore; rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
        rp.depthAttachment.texture = dst; rp.depthAttachment.loadAction = MTLLoadActionClear; rp.depthAttachment.storeAction = MTLStoreActionStore; rp.depthAttachment.clearDepth = 1.0;
        rp.stencilAttachment.texture = dst; rp.stencilAttachment.loadAction = MTLLoadActionClear; rp.stencilAttachment.storeAction = MTLStoreActionStore; rp.stencilAttachment.clearStencil = 0;
        id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:rp]; [re setRenderPipelineState:ps]; [re setViewport:(MTLViewport){ 0, 0, 16, 16, 0, 1 }];
        [re setDepthStencilState:wr]; [re setStencilReferenceValue:1]; dep_tri(re, 0, 0, 1, 1, 0.5f, RED);                       // stencil := 1 inside A
        [re setDepthStencilState:eq]; [re setStencilReferenceValue:1]; dep_tri(re, 0, 0, 2, 2, 0.5f, GREEN);                     // a larger triangle, drawn only where stencil == 1
        [re endEncoding];
        if (!n_run(cb, "stencil")) { printf("mtlprobe: FAIL depth (command buffer, stencil)\n"); return 2; }
        NSData *g = n_readback(dev, q, tgt, 16, 16, 4); if (!g) { printf("mtlprobe: FAIL depth (readback)\n"); return 2; }
        const uint8_t *in = dep_px(g, 0.20f, 0.20f), *out = dep_px(g, 0.70f, 0.60f);   // inside A (stencil 1) / inside the larger triangle but outside A
        printf("mtlprobe: depth stencil: inside A B%u G%u R%u, outside A B%u G%u R%u\n", in[0], in[1], in[2], out[0], out[1], out[2]);
        DEP_CHECK("stencil replace + equal", in[1] == 255 && in[2] == 0 && out[0] == 0 && out[1] == 0 && out[2] == 0, "the stencil-equal draw appears only where stencil was replaced with 1 (inside A green, outside A the clear colour)"); } }
    printf(fails == 0 ? "mtlprobe: PASS depth\n" : "mtlprobe: FAIL depth (%d)\n", fails);
    return fails ? 1 : 0;
}
// mtlprobe msaa: a 4-sample BGRA8 target (2DMultisample, Private), the unit triangle with a diagonal edge x == y, resolved into a 1-sample texture by the render pass (MultisampleResolve,
// StoreAndMultisampleResolve, and Unknown + setColorStoreAction:).  The interior (x < y) is exact, the exterior (x > y) is the clear colour, and the 16 diagonal pixels are intermediate.
static int msaa_one(id<MTLDevice> dev, id<MTLCommandQueue> q, id<MTLRenderPipelineState> pso, const char *name, int variant, int *fails) {
    #define MS_CHECK(tag, cond, ...) do { int ok_ = (cond) ? 1 : 0; printf("mtlprobe: msaa %s %s: %s: ", name, ok_ ? "ok  " : "FAIL", tag); printf(__VA_ARGS__); printf("\n"); if (!ok_) (*fails)++; } while (0)
    MTLTextureDescriptor *td = [MTLTextureDescriptor new]; td.textureType = MTLTextureType2DMultisample; td.pixelFormat = MTLPixelFormatBGRA8Unorm; td.width = 16; td.height = 16; td.sampleCount = 4; td.usage = MTLTextureUsageRenderTarget; td.storageMode = MTLStorageModePrivate;
    id<MTLTexture> ms = [dev newTextureWithDescriptor:td], rs = n_tex(dev, MTLPixelFormatBGRA8Unorm, 16, 16, MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead, MTLStorageModePrivate);
    MS_CHECK("multisample texture", ms && ms.textureType == MTLTextureType2DMultisample && ms.sampleCount == 4 && rs, "%s sampleCount %lu", ms ? "created" : "NIL", ms ? (unsigned long)ms.sampleCount : 0UL);
    if (!ms || !rs) return 0;
    id<MTLCommandBuffer> cb = [q commandBuffer]; MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = ms; rp.colorAttachments[0].resolveTexture = rs; rp.colorAttachments[0].loadAction = MTLLoadActionClear; rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
    rp.colorAttachments[0].storeAction = variant == 0 ? MTLStoreActionMultisampleResolve : variant == 1 ? MTLStoreActionStoreAndMultisampleResolve : MTLStoreActionUnknown;
    id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:rp]; [re setRenderPipelineState:pso]; [re setViewport:(MTLViewport){ 0, 0, 16, 16, 0, 1 }];
    if (variant == 2) [re setColorStoreAction:MTLStoreActionMultisampleResolve atIndex:0];
    const float RED[4] = { 1, 0, 0, 1 }; dep_tri(re, 0, 0, 1, 1, 0.5f, RED); [re endEncoding];
    if (!n_run(cb, "msaa")) { printf("mtlprobe: FAIL msaa (command buffer)\n"); return 0; }
    NSData *g = n_readback(dev, q, rs, 16, 16, 4); if (!g) { printf("mtlprobe: FAIL msaa (readback)\n"); return 0; }
    const uint8_t *px = g.bytes; int inner = 0, innerOk = 0, outer = 0, outerOk = 0, edge = 0, edgeMid = 0;
    for (int y = 0; y < 16; y++) for (int x = 0; x < 16; x++) { const uint8_t *p = px + (y * 16 + x) * 4;   // B G R A
        if (x < y) { inner++; innerOk += p[2] == 255 && p[1] == 0 && p[0] == 0; } else if (x > y) { outer++; outerOk += p[2] == 0 && p[1] == 0 && p[0] == 0; } else { edge++; edgeMid += p[2] > 10 && p[2] < 245 && p[1] == 0 && p[0] == 0; } }
    printf("mtlprobe: msaa %s: diagonal R values:", name); for (int i = 0; i < 16; i++) printf(" %u", px[(i * 16 + i) * 4 + 2]); printf("\n");
    MS_CHECK("interior", innerOk == inner, "%d of %d interior pixels (x < y) are exactly the triangle colour", innerOk, inner);
    MS_CHECK("exterior", outerOk == outer, "%d of %d exterior pixels (x > y) are exactly the clear colour", outerOk, outer);
    MS_CHECK("edge", edgeMid >= edge - 2, "%d of %d diagonal pixels hold an intermediate value (the resolve averaged the samples)", edgeMid, edge);
    return 1;
}
static int cmd_msaa(uint64_t rid, int haveRid) {
    id<MTLDevice> dev = pick_device(rid, haveRid); if (!dev) return 2;
    int fails = 0;
    printf("mtlprobe: msaa note: supportsTextureSampleCount 1/2/4/8 = %d/%d/%d/%d\n", (int)[dev supportsTextureSampleCount:1], (int)[dev supportsTextureSampleCount:2], (int)[dev supportsTextureSampleCount:4], (int)[dev supportsTextureSampleCount:8]);
    if (![dev supportsTextureSampleCount:4]) { printf("mtlprobe: FAIL msaa (the device does not support 4 samples)\n"); return 1; }
    id<MTLLibrary> lib = dep_lib(dev); id<MTLFunction> vs = lib ? [lib newFunctionWithName:@"dep_vs"] : nil, fs = lib ? [lib newFunctionWithName:@"dep_fs"] : nil;
    if (!vs || !fs) { printf("mtlprobe: FAIL msaa (library)\n"); return 2; }
    MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new]; pd.vertexFunction = vs; pd.fragmentFunction = fs; pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm; pd.rasterSampleCount = 4;
    NSError *err = nil; id<MTLRenderPipelineState> pso = [dev newRenderPipelineStateWithDescriptor:pd error:&err]; perr("newRenderPipelineState (msaa)", err);
    printf("mtlprobe: msaa %s: pipeline with rasterSampleCount 4: %s\n", pso ? "ok  " : "FAIL", pso ? "non-nil" : "NIL"); if (!pso) return 1;
    id<MTLCommandQueue> q = [dev newCommandQueue];
    static const char *nm[3] = { "MultisampleResolve", "StoreAndMultisampleResolve", "Unknown+setColorStoreAction" };
    for (int v = 0; v < 3; v++) if (!msaa_one(dev, q, pso, nm[v], v, &fails)) return 2;
    printf(fails == 0 ? "mtlprobe: PASS msaa\n" : "mtlprobe: FAIL msaa (%d)\n", fails);
    return fails ? 1 : 0;
}

// bundle 19 (missing menus, NATIVE-S8-MENUS.md): oracle for `mtlprobe rg16uint`.  Core Animation's large-shadow pass (brim_init / brim_jump / brim_outline) ping-pongs two RG16Uint targets
// (pixel format 63): a uint2 fragment output, an integer read() of the previous pass, and an integer clear.  The shaders below have the same shape.
// Metal's integer clear (measured on an Apple-silicon Mac, bundle 19): clearColor doubles are TRUNCATED toward zero and SATURATED to [0, 65535] per channel (NaN -> 0): (1.5, 2.5) -> (1, 2), (-5, 70000) -> (0, 65535), (255.9999, ..) -> 255.
static const char *kRgSrc =
"#include <metal_stdlib>\n"
"using namespace metal;\n"
"struct RgO { float4 pos [[position]]; };\n"
"vertex RgO rg_vs(uint vid [[vertex_id]]) { float2 c = float2(float(vid & 1u), float(vid >> 1)); RgO o; o.pos = float4(c * 2.0 - 1.0, 0.0, 1.0); return o; }\n"
"fragment uint2 rg_init(RgO in [[stage_in]]) { return uint2(uint(in.pos.x), uint(in.pos.y)); }\n"
"fragment uint2 rg_jump(RgO in [[stage_in]], texture2d<uint, access::read> src [[texture(0)]]) {\n"
"    uint2 p = uint2(uint(in.pos.x), uint(in.pos.y));\n"
"    uint2 v = src.read(uint2((p.x + 1u) & 255u, p.y)).xy;\n"
"    return uint2(v.y + 100u, v.x + 1000u); }\n";
static id<MTLLibrary> rg_lib(id<MTLDevice> dev) {
    NSError *err = nil; id<MTLLibrary> lib = [dev newLibraryWithSource:@(kRgSrc) options:nil error:&err];
    perr("newLibraryWithSource (rg16uint)", err);
    return lib;
}
static BOOL rg_clear_only(id<MTLDevice> dev, id<MTLCommandQueue> q, id<MTLTexture> t, double a, double b) {
    id<MTLCommandBuffer> cb = [q commandBuffer];
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = t; rp.colorAttachments[0].loadAction = MTLLoadActionClear; rp.colorAttachments[0].storeAction = MTLStoreActionStore; rp.colorAttachments[0].clearColor = MTLClearColorMake(a, b, 0, 0);
    id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:rp]; [re endEncoding];
    return n_run(cb, "rg16uint clear");
}
static int rg_count(NSData *d, int n, uint16_t x, uint16_t y) { const uint16_t *p = d.bytes; int bad = 0; for (int i = 0; i < n; i++) if (p[i * 2] != x || p[i * 2 + 1] != y) bad++; return bad; }
// mtlprobe rg16uint [--registry-id N]: bundle 19.  256x256 private RG16Uint targets; exact readback of (a) clear (12345, 65535), (b) a saturating / truncating clear (255.9999, 70000) -> (255, 65535),
// (c) a uint2(x, y) shader output, (d) a read() ping-pong pass: out(x, y) = (src(x+1 & 255, y).y + 100, src(...).x + 1000) = (y + 100, ((x+1) & 255) + 1000).
static int cmd_rg16uint(uint64_t rid, int haveRid) {
    id<MTLDevice> dev = pick_device(rid, haveRid); if (!dev) return 2;
    int fails = 0;
    #define RG_CHECK(tag, cond, ...) do { int ok_ = (cond) ? 1 : 0; printf("mtlprobe: rg16uint %s: %s: ", ok_ ? "ok  " : "FAIL", tag); printf(__VA_ARGS__); printf("\n"); if (!ok_) fails++; } while (0)
    const MTLTextureUsage U = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    id<MTLTexture> A = n_tex(dev, MTLPixelFormatRG16Uint, 256, 256, U, MTLStorageModePrivate), B = n_tex(dev, MTLPixelFormatRG16Uint, 256, 256, U, MTLStorageModePrivate);
    id<MTLTexture> C = n_tex(dev, MTLPixelFormatRG16Uint, 256, 256, U, MTLStorageModePrivate), D = n_tex(dev, MTLPixelFormatRG16Uint, 256, 256, U, MTLStorageModePrivate);
    RG_CHECK("create four 256x256 private RG16Uint targets", A && B && C && D, "%s", (A && B && C && D) ? "non-nil" : "NIL (pixel format 63 refused)");
    if (!(A && B && C && D)) { printf("mtlprobe: FAIL rg16uint (no texture)\n"); return 1; }
    RG_CHECK("properties", A.pixelFormat == MTLPixelFormatRG16Uint && A.width == 256 && A.height == 256, "pf %lu %lux%lu", (unsigned long)A.pixelFormat, (unsigned long)A.width, (unsigned long)A.height);
    id<MTLCommandQueue> q = [dev newCommandQueue];
    if (!rg_clear_only(dev, q, C, 12345, 65535)) { printf("mtlprobe: FAIL rg16uint (clear)\n"); return 2; }
    NSData *gc = n_readback(dev, q, C, 256, 256, 4); if (!gc) { printf("mtlprobe: FAIL rg16uint (readback)\n"); return 2; }
    int bc = rg_count(gc, 65536, 12345, 65535);
    RG_CHECK("clear (12345, 65535)", bc == 0, "%d of 65536 pixels differ; first = (%u, %u)", bc, ((const uint16_t *)gc.bytes)[0], ((const uint16_t *)gc.bytes)[1]);
    if (!rg_clear_only(dev, q, D, 255.9999, 70000)) { printf("mtlprobe: FAIL rg16uint (clear 2)\n"); return 2; }
    NSData *gd = n_readback(dev, q, D, 256, 256, 4); if (!gd) { printf("mtlprobe: FAIL rg16uint (readback)\n"); return 2; }
    int bd = rg_count(gd, 65536, 255, 65535);
    RG_CHECK("clear (255.9999, 70000) truncates and saturates", bd == 0, "%d of 65536 pixels differ; first = (%u, %u) expected (255, 65535)", bd, ((const uint16_t *)gd.bytes)[0], ((const uint16_t *)gd.bytes)[1]);
    id<MTLLibrary> lib = rg_lib(dev); id<MTLFunction> vs = lib ? [lib newFunctionWithName:@"rg_vs"] : nil, f0 = lib ? [lib newFunctionWithName:@"rg_init"] : nil, f1 = lib ? [lib newFunctionWithName:@"rg_jump"] : nil;
    RG_CHECK("library + functions", vs && f0 && f1, "vs %s init %s jump %s", vs ? "ok" : "NIL", f0 ? "ok" : "NIL", f1 ? "ok" : "NIL");
    if (!vs || !f0 || !f1) { printf("mtlprobe: FAIL rg16uint\n"); return 2; }
    NSError *err = nil; id<MTLRenderPipelineState> p0 = nil, p1 = nil;
    for (int k = 0; k < 2; k++) {
        MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new]; pd.vertexFunction = vs; pd.fragmentFunction = k ? f1 : f0; pd.colorAttachments[0].pixelFormat = MTLPixelFormatRG16Uint;
        id<MTLRenderPipelineState> pso = [dev newRenderPipelineStateWithDescriptor:pd error:&err]; perr(k ? "newRenderPipelineState (rg_jump)" : "newRenderPipelineState (rg_init)", err);
        if (k) p1 = pso; else p0 = pso;
    }
    RG_CHECK("pipelines with an RG16Uint colour attachment", p0 && p1, "init %s jump %s", p0 ? "non-nil" : "NIL", p1 ? "non-nil" : "NIL");
    if (!p0 || !p1) { printf("mtlprobe: FAIL rg16uint\n"); return 2; }
    id<MTLCommandBuffer> cb = [q commandBuffer];
    for (int k = 0; k < 2; k++) {
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = k ? B : A; rp.colorAttachments[0].loadAction = MTLLoadActionClear; rp.colorAttachments[0].storeAction = MTLStoreActionStore; rp.colorAttachments[0].clearColor = MTLClearColorMake(12345, 65535, 0, 0);
        id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:rp];
        [re setRenderPipelineState:k ? p1 : p0]; [re setViewport:(MTLViewport){ 0, 0, 256, 256, 0, 1 }]; [re setScissorRect:(MTLScissorRect){ 0, 0, 256, 256 }];
        if (k) [re setFragmentTexture:A atIndex:0];
        [re drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4]; [re endEncoding];
    }
    if (!n_run(cb, "rg16uint ping-pong")) { printf("mtlprobe: FAIL rg16uint (command buffer)\n"); return 2; }
    NSData *ga = n_readback(dev, q, A, 256, 256, 4), *gb = n_readback(dev, q, B, 256, 256, 4); if (!ga || !gb) { printf("mtlprobe: FAIL rg16uint (readback)\n"); return 2; }
    const uint16_t *pa = ga.bytes, *pb = gb.bytes; int badA = 0, badB = 0;
    for (int y = 0; y < 256; y++) for (int x = 0; x < 256; x++) {
        if (pa[(y * 256 + x) * 2] != x || pa[(y * 256 + x) * 2 + 1] != y) badA++;
        if (pb[(y * 256 + x) * 2] != y + 100 || pb[(y * 256 + x) * 2 + 1] != ((x + 1) & 255) + 1000) badB++; }
    RG_CHECK("uint2(x, y) fragment output", badA == 0, "%d of 65536 pixels differ; (3,5) = (%u, %u) expected (3, 5)", badA, pa[(5 * 256 + 3) * 2], pa[(5 * 256 + 3) * 2 + 1]);
    RG_CHECK("read() ping-pong pass", badB == 0, "%d of 65536 pixels differ; (3,5) = (%u, %u) expected (105, 1004); (255,7) = (%u, %u) expected (107, 1000)", badB, pb[(5 * 256 + 3) * 2], pb[(5 * 256 + 3) * 2 + 1], pb[(7 * 256 + 255) * 2], pb[(7 * 256 + 255) * 2 + 1]);
    printf(fails == 0 ? "mtlprobe: PASS rg16uint (exact)\n" : "mtlprobe: FAIL rg16uint (%d)\n", fails);
    return fails ? 1 : 0;
}

// mtlprobe dumpair <dir>: compile the 11e sources and write every function's bitcodeData to <dir>/<sha256>.air with
// <sha256>.txt (name, stage) so the host Mac can translate them (tools/native/navi48metal/add-air.py).
static int cmd_dumpair(const char *dir, uint64_t rid, int haveRid) {
    id<MTLDevice> dev = pick_device(rid, haveRid); if (!dev) return 2;
    id<MTLLibrary> lib0 = n_lib(dev); if (!lib0) return 2;
    id<MTLLibrary> lib1 = g2_lib(dev);   // m11h9: the texture-oracle shaders too
    id<MTLLibrary> lib2 = cube_lib(dev); // bundle 9: the cube oracle
    id<MTLLibrary> lib3 = dep_lib(dev);  // bundle 10: the depth / msaa oracle
    id<MTLLibrary> lib4 = rg_lib(dev);   // bundle 19: the RG16Uint oracle
    mkdir(dir, 0777);
    SEL bsel = NSSelectorFromString(@"bitcodeData"); int n = 0;
    for (int li = 0; li < 5; li++) for (NSString *nm in (li == 4 ? lib4 : li == 3 ? lib3 : li == 2 ? lib2 : li ? lib1 : lib0).functionNames) {
        id<MTLLibrary> lib = li == 4 ? lib4 : li == 3 ? lib3 : li == 2 ? lib2 : li ? lib1 : lib0;
        id<MTLFunction> fn = [lib newFunctionWithName:nm];
        NSData *bc = fn && [(id)fn respondsToSelector:bsel] ? ((NSData *(*)(id, SEL))objc_msgSend)(fn, bsel) : nil;
        if (!bc.length) { printf("dumpair: %s no bitcode\n", [nm UTF8String]); continue; }
        unsigned char h[CC_SHA256_DIGEST_LENGTH]; CC_SHA256(bc.bytes, (CC_LONG)bc.length, h);
        char hex[65]; for (int k = 0; k < 32; k++) snprintf(hex + 2 * k, 3, "%02x", h[k]);
        long st = (long)fn.functionType; const char *sn = st == 1 ? "vertex" : st == 2 ? "fragment" : st == 3 ? "kernel" : "other";
        NSString *base = [NSString stringWithFormat:@"%s/%s", dir, hex];
        [bc writeToFile:[base stringByAppendingString:@".air"] atomically:YES];
        [[NSString stringWithFormat:@"%@ %s\n", nm, sn] writeToFile:[base stringByAppendingString:@".txt"] atomically:YES encoding:NSUTF8StringEncoding error:NULL];
        printf("dumpair: %s %s %s %lu bytes\n", [nm UTF8String], sn, hex, (unsigned long)bc.length); n++;
    }
    printf("dumpair: %d functions written to %s\n", n, dir);
    return n ? 0 : 1;
}

// ---- (e) skydraw: SkyLight SimpleVertex + SimpleTextureFragment ----
// SimpleVertex (AIR): [[stage_in]] air.vertex_input location 0 = float2 _pos, location 1 = float2 _tex; buffer(1) = float4x4 mvp_matrix (constant, 64 B);
// out: position = mvp * (pos,0,1), user(generated) float2 tex. SimpleTextureFragment: texture(0) (sample), sampler(0), returns tex.sample(samp, tex).
static int cmd_skydraw(uint64_t rid, int haveRid, const char *out) {
    id<MTLDevice> dev = pick_device(rid, haveRid); if (!dev) return 2;
    NSError *err = nil;
    id<MTLLibrary> lib = [dev newLibraryWithURL:[NSURL fileURLWithPath:@"/System/Library/PrivateFrameworks/SkyLight.framework/Versions/A/Resources/SkyLightShaders.air64.metallib"] error:&err];
    perr("newLibraryWithURL", err);
    id<MTLFunction> vs = lib ? [lib newFunctionWithName:@"SimpleVertex"] : nil, fs = lib ? [lib newFunctionWithName:@"SimpleTextureFragment"] : nil;
    if (!vs || !fs) { printf("mtlprobe: FAIL SkyLight functions\n"); return 2; }
    MTLVertexDescriptor *vd = [MTLVertexDescriptor vertexDescriptor];
    vd.attributes[0].format = MTLVertexFormatFloat2; vd.attributes[0].offset = 0; vd.attributes[0].bufferIndex = 0;
    vd.attributes[1].format = MTLVertexFormatFloat2; vd.attributes[1].offset = 8; vd.attributes[1].bufferIndex = 0;
    vd.layouts[0].stride = 16; vd.layouts[0].stepFunction = MTLVertexStepFunctionPerVertex; vd.layouts[0].stepRate = 1;
    MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
    pd.vertexFunction = vs; pd.fragmentFunction = fs; pd.vertexDescriptor = vd; pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    id<MTLRenderPipelineState> pso = [dev newRenderPipelineStateWithDescriptor:pd error:&err]; perr("newRenderPipelineState", err);
    printf("mtlprobe: step pipeline: %s\n", pso ? "non-nil" : "NIL");
    if (!pso) { printf("mtlprobe: FAIL pipeline\n"); return 2; }
    MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new];
    sd.minFilter = MTLSamplerMinMagFilterNearest; sd.magFilter = MTLSamplerMinMagFilterNearest;
    id<MTLSamplerState> ss = [dev newSamplerStateWithDescriptor:sd];
    id<MTLTexture> src = n_source_texture(dev, MTLPixelFormatBGRA8Unorm, N_MANAGED);
    id<MTLTexture> tgt = n_tex(dev, MTLPixelFormatBGRA8Unorm, 64, 64, MTLTextureUsageRenderTarget, MTLStorageModePrivate);
    float verts[16] = { -1, 1, 0, 0,   1, 1, 1, 0,   -1, -1, 0, 1,   1, -1, 1, 1 };
    id<MTLBuffer> vb = [dev newBufferWithBytes:verts length:sizeof verts options:MTLResourceStorageModeShared];
    if (!ss || !src || !tgt || !vb) { printf("mtlprobe: FAIL alloc\n"); return 2; }
    float mvp[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
    id<MTLCommandQueue> q = [dev newCommandQueue]; id<MTLCommandBuffer> cb = [q commandBuffer];
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = tgt; rp.colorAttachments[0].loadAction = MTLLoadActionClear; rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    rp.colorAttachments[0].clearColor = MTLClearColorMake(1, 0, 1, 1);
    id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:rp];
    [re setRenderPipelineState:pso];
    [re setVertexBuffer:vb offset:0 atIndex:0];
    [re setVertexBytes:mvp length:sizeof mvp atIndex:1];
    [re setFragmentTexture:src atIndex:0];
    [re setFragmentSamplerState:ss atIndex:0];
    [re drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
    [re endEncoding];
    if (!n_run(cb, "skydraw")) { printf("mtlprobe: FAIL skydraw (command buffer)\n"); return 2; }
    NSData *got = n_readback(dev, q, tgt, 64, 64, 4); if (!got) { printf("mtlprobe: FAIL readback\n"); return 2; }
    uint8_t pat[64 * 64 * 4]; n_pattern(pat);
    static uint8_t exp[64 * 64 * 4];
    for (int i = 0; i < 64 * 64; i++) { exp[i*4] = pat[i*4+2]; exp[i*4+1] = pat[i*4+1]; exp[i*4+2] = pat[i*4]; exp[i*4+3] = pat[i*4+3]; }
    int diff = n_report_diff("skydraw SkyLight SimpleTextureFragment", got.bytes, exp, 64 * 64, 0, out, 64, 64);
    printf(diff == 0 ? "mtlprobe: PASS skydraw (exact)\n" : "mtlprobe: FAIL skydraw\n");
    return diff == 0 ? 0 : 1;
}

// ---- cull: cull mode x winding through the blend pipeline (vertex_id strips are counter-clockwise on screen) ----
static int cmd_cull(uint64_t rid, int haveRid, const char *out) {
    id<MTLDevice> dev = pick_device(rid, haveRid); if (!dev) return 2;
    id<MTLLibrary> lib = n_lib(dev); if (!lib) return 2;
    id<MTLFunction> vs = [lib newFunctionWithName:@"blend_vs"], fs = [lib newFunctionWithName:@"blend_fs"];
    MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
    pd.vertexFunction = vs; pd.fragmentFunction = fs; pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    NSError *err = nil; id<MTLRenderPipelineState> pso = [dev newRenderPipelineStateWithDescriptor:pd error:&err]; perr("newRenderPipelineState", err);
    id<MTLTexture> tgt = n_tex(dev, MTLPixelFormatBGRA8Unorm, W, H, MTLTextureUsageRenderTarget, MTLStorageModePrivate);
    if (!pso || !tgt) { printf("mtlprobe: FAIL setup\n"); return 2; }
    // five 64x64 squares at x = 8, 56, 104, 152, 200 (y 96..160): {cull, winding, expect drawn}
    struct { MTLCullMode cull; MTLWinding w; int drawn; const char *what; } cs[5] = {
        { MTLCullModeNone, MTLWindingClockwise, 1, "cull none" }, { MTLCullModeBack, MTLWindingClockwise, 0, "cull back, CW front (strip is CCW = back)" },
        { MTLCullModeBack, MTLWindingCounterClockwise, 1, "cull back, CCW front" }, { MTLCullModeFront, MTLWindingClockwise, 1, "cull front, CW front (strip is back: kept)" },
        { MTLCullModeFront, MTLWindingCounterClockwise, 0, "cull front, CCW front (strip is front: culled)" } };
    id<MTLCommandQueue> q = [dev newCommandQueue]; id<MTLCommandBuffer> cb = [q commandBuffer];
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = tgt; rp.colorAttachments[0].loadAction = MTLLoadActionClear; rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
    id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:rp];
    [re setRenderPipelineState:pso];
    float col[4] = { 1, 1, 1, 1 };
    for (int k = 0; k < 5; k++) {
        int x0 = 8 + 48 * k, y0 = 96;
        float rect[4] = { (x0 + 8) / 128.0f - 1.0f, 1.0f - (y0 + 64) / 128.0f, 48 / 128.0f, 64 / 128.0f };
        [re setCullMode:cs[k].cull]; [re setFrontFacingWinding:cs[k].w];
        [re setVertexBytes:rect length:sizeof rect atIndex:0]; [re setFragmentBytes:col length:sizeof col atIndex:0];
        [re drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
    }
    [re endEncoding];
    if (!n_run(cb, "cull")) { printf("mtlprobe: FAIL cull (command buffer)\n"); return 2; }
    NSData *got = n_readback(dev, q, tgt, W, H, 4); if (!got) return 2;
    static uint8_t exp[W * H * 4];
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        uint8_t v = 0;
        for (int k = 0; k < 5; k++) { int x0 = 16 + 48 * k; if (cs[k].drawn && x >= x0 && x < x0 + 48 && y >= 96 && y < 160) v = 255; }
        uint8_t *e = exp + (y * W + x) * 4; e[0] = e[1] = e[2] = v; e[3] = 255;
    }
    for (int k = 0; k < 5; k++) { int x0 = 16 + 48 * k; const uint8_t *p = (const uint8_t *)got.bytes + (128 * W + x0 + 24) * 4;
        printf("mtlprobe: square %d (%s): centre pixel = %u (expected %s)\n", k, cs[k].what, p[0], cs[k].drawn ? "255 drawn" : "0 culled"); }
    int diff = n_report_diff("cull/winding", got.bytes, exp, W * H, 0, out, W, H);
    printf(diff == 0 ? "mtlprobe: PASS cull (exact)\n" : "mtlprobe: FAIL cull\n");
    return diff == 0 ? 0 : 1;
}

// ---- extra: storage modes, blits, private buffer, encode-error paths ----
static int cmd_extra(uint64_t rid, int haveRid) {
    id<MTLDevice> dev = pick_device(rid, haveRid); if (!dev) return 2;
    int fails = 0;
    struct { MTLStorageMode m; const char *n; } modes[3] = { { MTLStorageModeShared, "Shared" }, { N_MANAGED, "Managed" }, { MTLStorageModePrivate, "Private" } };
    for (int k = 0; k < 3; k++) {
        id<MTLTexture> t = n_tex(dev, MTLPixelFormatRGBA8Unorm, 8, 8, MTLTextureUsageShaderRead, modes[k].m);
        if (!t) { printf("mtlprobe: extra: %s texture creation NIL\n", modes[k].n); if (modes[k].m != MTLStorageModePrivate || 1) fails++; continue; }
        uint8_t in[8 * 8 * 4], outb[8 * 8 * 4]; for (int i = 0; i < 256; i++) in[i] = (uint8_t)(i * 7 + 3); memset(outb, 0x5A, sizeof outb);
        [t replaceRegion:MTLRegionMake2D(0, 0, 8, 8) mipmapLevel:0 withBytes:in bytesPerRow:32];
        [t getBytes:outb bytesPerRow:32 fromRegion:MTLRegionMake2D(0, 0, 8, 8) mipmapLevel:0];
        int same = !memcmp(in, outb, sizeof in), untouched = outb[0] == 0x5A && outb[255] == 0x5A;
        int ok = modes[k].m == MTLStorageModePrivate ? untouched : same;
        printf("mtlprobe: extra: replaceRegion+getBytes on %s texture: %s (%s)\n", modes[k].n, same ? "round-trip exact" : untouched ? "refused (no-op)" : "MISMATCH", ok ? "as designed" : "UNEXPECTED");
        if (!ok) fails++;
    }
    id<MTLCommandQueue> q = [dev newCommandQueue];
    // Private buffer + buffer->buffer blits + buffer->texture->texture->buffer
    id<MTLBuffer> a = [dev newBufferWithLength:256 options:MTLResourceStorageModeShared], p = [dev newBufferWithLength:256 options:MTLResourceStorageModePrivate], c = [dev newBufferWithLength:256 options:MTLResourceStorageModeShared];
    id<MTLBuffer> bb = [dev newBufferWithBytes:"0123456789abcdef" length:16 options:MTLResourceStorageModeShared];
    printf("mtlprobe: extra: Private buffer %s contents=%s; newBufferWithBytes contents %s\n", p ? "non-nil" : "NIL", p && p.contents ? "non-NULL" : "NULL", bb && !memcmp(bb.contents, "0123456789abcdef", 16) ? "exact" : "WRONG");
    if (!a || !p || !c || !bb || (p && p.contents) || !bb.contents || memcmp(bb.contents, "0123456789abcdef", 16)) fails++;
    id<MTLTexture> t1 = n_tex(dev, MTLPixelFormatRGBA8Unorm, 8, 8, MTLTextureUsageShaderRead, MTLStorageModePrivate), t2 = n_tex(dev, MTLPixelFormatRGBA8Unorm, 8, 8, MTLTextureUsageShaderRead, MTLStorageModePrivate);
    for (int i = 0; i < 256; i++) ((uint8_t *)a.contents)[i] = (uint8_t)(i * 5 + 1);
    memset(c.contents, 0, 256);
    id<MTLCommandBuffer> cb = [q commandBuffer]; id<MTLBlitCommandEncoder> be = [cb blitCommandEncoder];
    [be copyFromBuffer:a sourceOffset:0 toBuffer:p destinationOffset:0 size:256];
    [be copyFromBuffer:p sourceOffset:0 sourceBytesPerRow:32 sourceBytesPerImage:256 sourceSize:MTLSizeMake(8, 8, 1) toTexture:t1 destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
    [be copyFromTexture:t1 sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(8, 8, 1) toTexture:t2 destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
    [be copyFromTexture:t2 sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(8, 8, 1) toBuffer:c destinationOffset:0 destinationBytesPerRow:32 destinationBytesPerImage:256];
    [be endEncoding];
    BOOL ok1 = n_run(cb, "blit chain");
    int same = ok1 && !memcmp(a.contents, c.contents, 256);
    printf("mtlprobe: extra: shared -> private buffer -> texture -> texture -> shared buffer: %s\n", same ? "round-trip exact" : "MISMATCH"); if (!same) fails++;
    // encode-error paths: each must complete with status Error (5) and an NSError, never hang or crash
    id<MTLLibrary> lib = n_lib(dev); NSError *err = nil;
    id<MTLComputePipelineState> pa = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"fill_buf"] error:&err];
    id<MTLBuffer> ob = [dev newBufferWithLength:4096 options:MTLResourceStorageModeShared];
    cb = [q commandBuffer]; id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
    uint32_t n = 100; [ce setComputePipelineState:pa]; [ce setBuffer:ob offset:0 atIndex:0]; [ce setBytes:&n length:4 atIndex:1];
    [ce dispatchThreadgroups:MTLSizeMake(4, 1, 1) threadsPerThreadgroup:MTLSizeMake(32, 1, 1)];   // local size mismatch (module is 64,1,1)
    [ce endEncoding]; [cb commit]; [cb waitUntilCompleted];
    int e1 = cb.status == MTLCommandBufferStatusError && cb.error && strstr([[cb.error localizedDescription] UTF8String], "threadsPerThreadgroup");
    printf("mtlprobe: extra: compute local-size mismatch: status=%ld error=%s -> %s\n", (long)cb.status, cb.error ? [[cb.error localizedDescription] UTF8String] : "(none)", e1 ? "clean NSError" : "UNEXPECTED"); if (!e1) fails++;
    id<MTLFunction> qv = [lib newFunctionWithName:@"quad_vs"], qf = [lib newFunctionWithName:@"quad_fs"];
    MTLVertexDescriptor *vd = [MTLVertexDescriptor vertexDescriptor];
    vd.attributes[0].format = MTLVertexFormatFloat2; vd.attributes[0].offset = 0; vd.attributes[0].bufferIndex = 0;
    vd.attributes[1].format = MTLVertexFormatFloat2; vd.attributes[1].offset = 8; vd.attributes[1].bufferIndex = 0; vd.layouts[0].stride = 16;
    MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
    pd.vertexFunction = qv; pd.fragmentFunction = qf; pd.vertexDescriptor = vd; pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    id<MTLRenderPipelineState> pso = [dev newRenderPipelineStateWithDescriptor:pd error:&err];
    id<MTLTexture> tgt = n_tex(dev, MTLPixelFormatBGRA8Unorm, 16, 16, MTLTextureUsageRenderTarget, MTLStorageModePrivate);
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = tgt; rp.colorAttachments[0].loadAction = MTLLoadActionClear; rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    cb = [q commandBuffer]; id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:rp];   // draw with the vertex buffer never bound
    [re setRenderPipelineState:pso]; [re drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3]; [re endEncoding]; [cb commit]; [cb waitUntilCompleted];
    int e2 = cb.status == MTLCommandBufferStatusError && cb.error && strstr([[cb.error localizedDescription] UTF8String], "vertex buffer");
    printf("mtlprobe: extra: draw with an unbound vertex buffer: status=%ld error=%s -> %s\n", (long)cb.status, cb.error ? [[cb.error localizedDescription] UTF8String] : "(none)", e2 ? "clean NSError" : "UNEXPECTED"); if (!e2) fails++;
    // unbound fragment texture/sampler/buffer: dummies, the command buffer must still complete
    id<MTLBuffer> vb = [dev newBufferWithLength:64 options:MTLResourceStorageModeShared]; memset(vb.contents, 0, 64);
    cb = [q commandBuffer]; re = [cb renderCommandEncoderWithDescriptor:rp];
    float xf[4] = { 1, 1, 0, 0 }; [re setRenderPipelineState:pso]; [re setVertexBuffer:vb offset:0 atIndex:0]; [re setVertexBytes:xf length:16 atIndex:1];
    [re drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3]; [re endEncoding];
    BOOL ok3 = n_run(cb, "unbound fragment resources");
    printf("mtlprobe: extra: draw with unbound fragment texture/sampler/buffer (dummies): %s\n", ok3 ? "completed" : "FAILED"); if (!ok3) fails++;
    printf("mtlprobe: extra: %d unexpected results\n", fails);
    printf(fails == 0 ? "mtlprobe: PASS extra\n" : "mtlprobe: FAIL extra\n");
    return fails ? 1 : 0;
}

// ---- passbreak: layout transitions between draws (render target -> sampled, compute-written storage image -> sampled) in one command buffer ----
static int cmd_passbreak(uint64_t rid, int haveRid, const char *out) {
    id<MTLDevice> dev = pick_device(rid, haveRid); if (!dev) return 2;
    id<MTLLibrary> lib = n_lib(dev); if (!lib) return 2;
    NSError *err = nil;
    MTLVertexDescriptor *vd = [MTLVertexDescriptor vertexDescriptor];
    vd.attributes[0].format = MTLVertexFormatFloat2; vd.attributes[0].offset = 0; vd.attributes[0].bufferIndex = 0;
    vd.attributes[1].format = MTLVertexFormatFloat2; vd.attributes[1].offset = 8; vd.attributes[1].bufferIndex = 0; vd.layouts[0].stride = 16;
    MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
    pd.vertexFunction = [lib newFunctionWithName:@"quad_vs"]; pd.fragmentFunction = [lib newFunctionWithName:@"quad_fs"]; pd.vertexDescriptor = vd;
    pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    id<MTLRenderPipelineState> pso = [dev newRenderPipelineStateWithDescriptor:pd error:&err]; perr("newRenderPipelineState", err);
    id<MTLComputePipelineState> cps = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"write_tex"] error:&err]; perr("compute pipeline", err);
    MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new]; sd.minFilter = MTLSamplerMinMagFilterNearest; sd.magFilter = MTLSamplerMinMagFilterNearest;
    id<MTLSamplerState> ss = [dev newSamplerStateWithDescriptor:sd];
    id<MTLTexture> src = n_source_texture(dev, MTLPixelFormatRGBA8Unorm, N_MANAGED);
    id<MTLTexture> R = n_tex(dev, MTLPixelFormatBGRA8Unorm, 64, 64, MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead, MTLStorageModePrivate);
    id<MTLTexture> C = n_tex(dev, MTLPixelFormatRGBA8Unorm, 64, 64, MTLTextureUsageShaderWrite | MTLTextureUsageShaderRead, MTLStorageModePrivate);
    id<MTLTexture> T = n_tex(dev, MTLPixelFormatBGRA8Unorm, W, H, MTLTextureUsageRenderTarget, MTLStorageModePrivate);
    float verts[16] = { -1, 1, 0, 0,   1, 1, 1, 0,   -1, -1, 0, 1,   1, -1, 1, 1 }; uint16_t idx[6] = { 0, 1, 2, 2, 1, 3 };
    id<MTLBuffer> vb = [dev newBufferWithBytes:verts length:sizeof verts options:MTLResourceStorageModeShared], ib = [dev newBufferWithBytes:idx length:sizeof idx options:MTLResourceStorageModeShared];
    if (!pso || !cps || !ss || !src || !R || !C || !T || !vb || !ib) { printf("mtlprobe: FAIL setup\n"); return 2; }
    id<MTLCommandQueue> q = [dev newCommandQueue]; id<MTLCommandBuffer> cb = [q commandBuffer];
    MTLRenderPassDescriptor *rp1 = [MTLRenderPassDescriptor renderPassDescriptor];
    rp1.colorAttachments[0].texture = R; rp1.colorAttachments[0].loadAction = MTLLoadActionClear; rp1.colorAttachments[0].storeAction = MTLStoreActionStore;
    rp1.colorAttachments[0].clearColor = MTLClearColorMake(200.0 / 255, 100.0 / 255, 50.0 / 255, 1.0);
    id<MTLRenderCommandEncoder> e1 = [cb renderCommandEncoderWithDescriptor:rp1]; [e1 endEncoding];   // clear-only encoder
    id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
    [ce setComputePipelineState:cps]; [ce setTexture:C atIndex:0];
    [ce dispatchThreadgroups:MTLSizeMake(8, 8, 1) threadsPerThreadgroup:MTLSizeMake(8, 8, 1)]; [ce endEncoding];
    MTLRenderPassDescriptor *rp2 = [MTLRenderPassDescriptor renderPassDescriptor];
    rp2.colorAttachments[0].texture = T; rp2.colorAttachments[0].loadAction = MTLLoadActionClear; rp2.colorAttachments[0].storeAction = MTLStoreActionStore;
    rp2.colorAttachments[0].clearColor = MTLClearColorMake(10.0 / 255, 20.0 / 255, 30.0 / 255, 1.0);
    id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:rp2];
    float tint[4] = { 1, 1, 1, 1 };
    float xf[3][4] = { { 0.5f, 0.5f, -0.5f, 0.5f }, { 0.5f, 0.5f, 0.5f, -0.5f }, { 0.5f, 0.5f, -0.5f, -0.5f } };   // TL, BR, BL
    id<MTLTexture> srcs[3] = { src, R, C };
    [re setRenderPipelineState:pso]; [re setVertexBuffer:vb offset:0 atIndex:0]; [re setFragmentBytes:tint length:sizeof tint atIndex:0]; [re setFragmentSamplerState:ss atIndex:0];
    for (int k = 0; k < 3; k++) {
        [re setVertexBytes:xf[k] length:16 atIndex:1]; [re setFragmentTexture:srcs[k] atIndex:0];
        [re drawIndexedPrimitives:MTLPrimitiveTypeTriangle indexCount:6 indexType:MTLIndexTypeUInt16 indexBuffer:ib indexBufferOffset:0];
    }
    [re endEncoding];
    if (!n_run(cb, "passbreak")) { printf("mtlprobe: FAIL passbreak (command buffer)\n"); return 2; }
    NSData *got = n_readback(dev, q, T, W, H, 4); if (!got) return 2;
    static uint8_t exp[W * H * 4]; uint8_t pat[64 * 64 * 4]; n_pattern(pat);
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        uint8_t *e = exp + (y * W + x) * 4; e[0] = 30; e[1] = 20; e[2] = 10; e[3] = 255;
        int tx = (x % 128) / 2, ty = (y % 128) / 2;
        if (x < 128 && y < 128) { const uint8_t *t = pat + (ty * 64 + tx) * 4; e[0] = t[2]; e[1] = t[1]; e[2] = t[0]; e[3] = t[3]; }
        else if (x >= 128 && y >= 128) { e[0] = 50; e[1] = 100; e[2] = 200; e[3] = 255; }
        else if (x < 128 && y >= 128) { e[2] = (uint8_t)tx; e[1] = (uint8_t)ty; e[0] = (uint8_t)((tx * 3 + ty) & 255); e[3] = 255; }
    }
    int diff = n_report_diff("passbreak (pattern | render-target-as-texture | compute-written texture)", got.bytes, exp, W * H, 0, out, W, H);
    printf(diff == 0 ? "mtlprobe: PASS passbreak (exact)\n" : "mtlprobe: FAIL passbreak\n");
    return diff == 0 ? 0 : 1;
}


// ---------------------------------------------------------------------------------------------------------------
// #11 step 11e-2: framebuffer fetch ([[color(n)]]) oracles: fbfetch (own MSL), fbsover (SkyLight InPlaceSover), fbcopy (QuartzCore inplace_copy_lpf).
// ---------------------------------------------------------------------------------------------------------------
static id<MTLRenderPipelineState> fb_pso(id<MTLDevice> dev, id<MTLFunction> vs, id<MTLFunction> fs, MTLVertexDescriptor *vd, const char *tag) {
    MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
    pd.vertexFunction = vs; pd.fragmentFunction = fs; pd.vertexDescriptor = vd; pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    NSError *err = nil; id<MTLRenderPipelineState> pso = [dev newRenderPipelineStateWithDescriptor:pd error:&err]; perr("newRenderPipelineState", err);
    printf("mtlprobe: step pipeline %s: %s\n", tag, pso ? "non-nil" : "NIL");
    return pso;
}
// CPU model of one draw over a BGRA8 image: dst' = quantise(dst * k + c) per logical RGBA channel, inside [x0,x1)x[y0,y1).
static void fb_cpu_affine(uint8_t *img, int w, int x0, int y0, int x1, int y1, const float k[4], const float c[4]) {
    static const int bi[4] = { 2, 1, 0, 3 };   // logical R,G,B,A -> BGRA byte index
    for (int y = y0; y < y1; y++) for (int x = x0; x < x1; x++) {
        uint8_t *p = img + (y * w + x) * 4;
        for (int j = 0; j < 4; j++) { float v = (p[bi[j]] / 255.0f) * k[j] + c[j]; v = v < 0 ? 0 : v > 1 ? 1 : v; p[bi[j]] = (uint8_t)lrintf(v * 255.0f); }
    }
}
static void fb_cpu_fill(uint8_t *img, int w, int x0, int y0, int x1, int y1, const float rgba[4]) {
    for (int y = y0; y < y1; y++) for (int x = x0; x < x1; x++) {
        uint8_t *p = img + (y * w + x) * 4;
        p[2] = (uint8_t)lrintf(rgba[0] * 255.0f); p[1] = (uint8_t)lrintf(rgba[1] * 255.0f); p[0] = (uint8_t)lrintf(rgba[2] * 255.0f); p[3] = (uint8_t)lrintf(rgba[3] * 255.0f);
    }
}
static void fb_rect(float r[4], int x0, int y0, int x1, int y1, int size) { r[0] = x0 * 2.0f / size - 1.0f; r[1] = 1.0f - y1 * 2.0f / size; r[2] = (x1 - x0) * 2.0f / size; r[3] = (y1 - y0) * 2.0f / size; }

// (a) fbfetch: clear, plain quad, then a sequence of fetch draws (dst*k+c) mixed with plain draws (pass shape switches both ways).
static int cmd_fbfetch(uint64_t rid, int haveRid, const char *out) {
    id<MTLDevice> dev = pick_device(rid, haveRid); if (!dev) return 2;
    id<MTLLibrary> lib = n_lib(dev); if (!lib) return 2;
    id<MTLFunction> vs = [lib newFunctionWithName:@"blend_vs"], fsp = [lib newFunctionWithName:@"blend_fs"], fsf = [lib newFunctionWithName:@"fb_fs"];
    if (!vs || !fsp || !fsf) { printf("mtlprobe: FAIL functions\n"); return 2; }
    id<MTLRenderPipelineState> plain = fb_pso(dev, vs, fsp, nil, "plain"), fetch = fb_pso(dev, vs, fsf, nil, "fetch");
    if (!plain || !fetch) { printf("mtlprobe: FAIL pipeline\n"); return 2; }
    id<MTLTexture> tgt = n_tex(dev, MTLPixelFormatBGRA8Unorm, W, H, MTLTextureUsageRenderTarget, MTLStorageModePrivate);
    if (!tgt) { printf("mtlprobe: FAIL alloc\n"); return 2; }
    enum { PLAIN, FETCH };
    static const float INV_K[4] = { -1, -1, -1, 1 }, INV_C[4] = { 1, 1, 1, 0 }, K2[4] = { 0.5f, 0.5f, 0.5f, 1 }, C2[4] = { 33.25f / 255, 17.25f / 255, 53.25f / 255, 0 };   // results end in .25/.75: far from a rounding tie (see rndprobe)
    static const float colA[4] = { 200 / 255.0f, 120 / 255.0f, 30 / 255.0f, 1 }, colD[4] = { 10 / 255.0f, 250 / 255.0f, 90 / 255.0f, 1 };
    struct { int kind, x0, y0, x1, y1; const float *a, *b; } seq[] = {
        { PLAIN, 40, 40, 168, 168, colA, NULL }, { FETCH, 88, 88, 216, 216, INV_K, INV_C }, { FETCH, 20, 100, 150, 230, K2, C2 },
        { FETCH, 100, 100, 180, 180, INV_K, INV_C }, { PLAIN, 120, 20, 200, 120, colD, NULL }, { FETCH, 150, 60, 240, 140, K2, C2 },
        { FETCH, 30, 30, 230, 230, INV_K, INV_C } };
    int nseq = (int)(sizeof seq / sizeof seq[0]);
    id<MTLCommandQueue> q = [dev newCommandQueue]; id<MTLCommandBuffer> cb = [q commandBuffer];
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = tgt; rp.colorAttachments[0].loadAction = MTLLoadActionClear; rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    rp.colorAttachments[0].clearColor = MTLClearColorMake(20.0 / 255, 40.0 / 255, 60.0 / 255, 1.0);
    id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:rp];
    for (int i = 0; i < nseq; i++) {
        float rect[4]; fb_rect(rect, seq[i].x0, seq[i].y0, seq[i].x1, seq[i].y1, 256);
        [re setRenderPipelineState:seq[i].kind == FETCH ? fetch : plain];
        [re setVertexBytes:rect length:sizeof rect atIndex:0];
        [re setFragmentBytes:seq[i].a length:16 atIndex:0];
        if (seq[i].kind == FETCH) [re setFragmentBytes:seq[i].b length:16 atIndex:1];
        [re drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
    }
    [re endEncoding];
    if (!n_run(cb, "fbfetch")) { printf("mtlprobe: FAIL fbfetch (command buffer)\n"); return 2; }
    NSData *got = n_readback(dev, q, tgt, W, H, 4); if (!got) { printf("mtlprobe: FAIL readback\n"); return 2; }
    static uint8_t exp[W * H * 4];
    for (int i = 0; i < W * H; i++) { exp[i*4] = 60; exp[i*4+1] = 40; exp[i*4+2] = 20; exp[i*4+3] = 255; }
    for (int i = 0; i < nseq; i++) {
        if (seq[i].kind == PLAIN) fb_cpu_fill(exp, W, seq[i].x0, seq[i].y0, seq[i].x1, seq[i].y1, seq[i].a);
        else fb_cpu_affine(exp, W, seq[i].x0, seq[i].y0, seq[i].x1, seq[i].y1, seq[i].a, seq[i].b);
    }
    int d1 = n_report_diff("fbfetch (tolerance 1 LSB)", got.bytes, exp, W * H, 1, out, W, H);
    int d0 = n_report_diff("fbfetch (tolerance 0)", got.bytes, exp, W * H, 0, NULL, W, H);
    printf(d0 == 0 ? "mtlprobe: PASS fbfetch (exact)\n" : d1 == 0 ? "mtlprobe: PASS fbfetch (within 1 LSB)\n" : "mtlprobe: FAIL fbfetch\n");
    return d1 == 0 ? 0 : 1;
}

// (b) fbsover: SkyLight InPlaceSover (AIR: (position, tex, float4 d0 = [[color(0)]], constant {float4 _color} at buffer(0)); returns (1 - _color.a) * d0 + _color)
// after a SimpleVertex + SimpleTextureFragment pass laid the 64x64 pattern down (SimpleVertex: attributes 0/1 float2, mvp at buffer(1)).
static int cmd_fbsover(uint64_t rid, int haveRid, const char *out) {
    id<MTLDevice> dev = pick_device(rid, haveRid); if (!dev) return 2;
    NSError *err = nil;
    id<MTLLibrary> lib = [dev newLibraryWithURL:[NSURL fileURLWithPath:@"/System/Library/PrivateFrameworks/SkyLight.framework/Versions/A/Resources/SkyLightShaders.air64.metallib"] error:&err];
    perr("newLibraryWithURL", err);
    id<MTLFunction> vs = lib ? [lib newFunctionWithName:@"SimpleVertex"] : nil, fst = lib ? [lib newFunctionWithName:@"SimpleTextureFragment"] : nil, fss = lib ? [lib newFunctionWithName:@"InPlaceSover"] : nil;
    if (!vs || !fst || !fss) { printf("mtlprobe: FAIL SkyLight functions\n"); return 2; }
    MTLVertexDescriptor *vd = [MTLVertexDescriptor vertexDescriptor];
    vd.attributes[0].format = MTLVertexFormatFloat2; vd.attributes[0].offset = 0; vd.attributes[0].bufferIndex = 0;
    vd.attributes[1].format = MTLVertexFormatFloat2; vd.attributes[1].offset = 8; vd.attributes[1].bufferIndex = 0;
    vd.layouts[0].stride = 16; vd.layouts[0].stepFunction = MTLVertexStepFunctionPerVertex; vd.layouts[0].stepRate = 1;
    id<MTLRenderPipelineState> ptex = fb_pso(dev, vs, fst, vd, "SimpleTextureFragment"), psov = fb_pso(dev, vs, fss, vd, "InPlaceSover");
    if (!ptex || !psov) { printf("mtlprobe: FAIL pipeline\n"); return 2; }
    MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new]; sd.minFilter = MTLSamplerMinMagFilterNearest; sd.magFilter = MTLSamplerMinMagFilterNearest;
    id<MTLSamplerState> ss = [dev newSamplerStateWithDescriptor:sd];
    id<MTLTexture> src = n_source_texture(dev, MTLPixelFormatBGRA8Unorm, N_MANAGED);
    id<MTLTexture> tgt = n_tex(dev, MTLPixelFormatBGRA8Unorm, 64, 64, MTLTextureUsageRenderTarget, MTLStorageModePrivate);
    struct { int x0, y0, x1, y1; float c[4]; } sov[3] = { { 8, 8, 40, 40, { 53.25f / 255, 28.25f / 255, 15.25f / 255, 0.25f } }, { 24, 16, 56, 48, { 7.25f / 255, 79.25f / 255, 28.25f / 255, 0.5f } }, { 0, 30, 64, 60, { 13.25f / 255, 5.25f / 255, 41.25f / 255, 0.5f } } };   // c * 255 ends in .25: results are far from a rounding tie (see rndprobe)
    float verts[16 * 4];   // quad 0 = full target with texcoords; quads 1..3 = the source-over rects (tex unused)
    { float full[16] = { -1, 1, 0, 0,   1, 1, 1, 0,   -1, -1, 0, 1,   1, -1, 1, 1 }; memcpy(verts, full, sizeof full); }
    for (int k = 0; k < 3; k++) {
        float x0 = sov[k].x0 / 32.0f - 1, x1 = sov[k].x1 / 32.0f - 1, y0 = 1 - sov[k].y0 / 32.0f, y1 = 1 - sov[k].y1 / 32.0f;
        float r[16] = { x0, y0, 0, 0,   x1, y0, 0, 0,   x0, y1, 0, 0,   x1, y1, 0, 0 }; memcpy(verts + 16 * (k + 1), r, sizeof r);
    }
    id<MTLBuffer> vb = [dev newBufferWithBytes:verts length:sizeof verts options:MTLResourceStorageModeShared];
    if (!ss || !src || !tgt || !vb) { printf("mtlprobe: FAIL alloc\n"); return 2; }
    float mvp[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
    id<MTLCommandQueue> q = [dev newCommandQueue]; id<MTLCommandBuffer> cb = [q commandBuffer];
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = tgt; rp.colorAttachments[0].loadAction = MTLLoadActionClear; rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    rp.colorAttachments[0].clearColor = MTLClearColorMake(1, 0, 1, 1);
    id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:rp];
    [re setVertexBuffer:vb offset:0 atIndex:0]; [re setVertexBytes:mvp length:sizeof mvp atIndex:1];
    [re setRenderPipelineState:ptex]; [re setFragmentTexture:src atIndex:0]; [re setFragmentSamplerState:ss atIndex:0];
    [re drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
    [re setRenderPipelineState:psov];
    for (int k = 0; k < 3; k++) {
        [re setFragmentBytes:sov[k].c length:16 atIndex:0];
        [re drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:4 * (k + 1) vertexCount:4];
    }
    [re endEncoding];
    if (!n_run(cb, "fbsover")) { printf("mtlprobe: FAIL fbsover (command buffer)\n"); return 2; }
    NSData *got = n_readback(dev, q, tgt, 64, 64, 4); if (!got) { printf("mtlprobe: FAIL readback\n"); return 2; }
    uint8_t pat[64 * 64 * 4]; n_pattern(pat);
    static uint8_t exp[64 * 64 * 4];
    for (int i = 0; i < 64 * 64; i++) { exp[i*4] = pat[i*4+2]; exp[i*4+1] = pat[i*4+1]; exp[i*4+2] = pat[i*4]; exp[i*4+3] = pat[i*4+3]; }
    for (int k = 0; k < 3; k++) {   // dst' = (1 - a) * dst + c  ==  dst * (1 - a) + c
        float kk[4] = { 1 - sov[k].c[3], 1 - sov[k].c[3], 1 - sov[k].c[3], 1 - sov[k].c[3] };
        fb_cpu_affine(exp, 64, sov[k].x0, sov[k].y0, sov[k].x1, sov[k].y1, kk, sov[k].c);
    }
    {   // informational: where the single-draw region (only the first source-over rect covers it) rounds, by the fractional part of the exact value 0.75*q + c*255
        const uint8_t *gp = got.bytes; int up[20] = {0}, dn[20] = {0}; static const int bi[3] = { 2, 1, 0 };
        for (int y = 8; y < 30; y++) for (int x = 8; x < 24; x++) for (int j = 0; j < 3; j++) {
            double ex = (double)pat[(y * 64 + x) * 4 + j] * (double)(1.0f - sov[0].c[3]) + (double)sov[0].c[j] * 255.0; double fl = floor(ex); int bin = (int)((ex - fl) * 20);
            int gv = gp[(y * 64 + x) * 4 + bi[j]]; if (gv > fl) up[bin]++; else dn[bin]++; }
        printf("mtlprobe: fbsover rounding by fraction of the exact value (single-draw region), bins of 0.05: up/down counts:");
        for (int b = 0; b < 20; b++) if (up[b] + dn[b]) printf(" [%.2f]%d/%d", b * 0.05, up[b], dn[b]);
        printf("\n");
    }
    int d1 = n_report_diff("fbsover SkyLight InPlaceSover (tolerance 1 LSB)", got.bytes, exp, 64 * 64, 1, out, 64, 64);
    int d0 = n_report_diff("fbsover SkyLight InPlaceSover (tolerance 0)", got.bytes, exp, 64 * 64, 0, NULL, 64, 64);
    printf(d0 == 0 ? "mtlprobe: PASS fbsover (exact)\n" : d1 == 0 ? "mtlprobe: PASS fbsover (within 1 LSB)\n" : "mtlprobe: FAIL fbsover\n");
    return d1 == 0 ? 0 : 1;
}

// (c) fbcopy: QuartzCore inplace_copy_lpf (AIR: (position, color, texcoord0_0, float4 d0 = [[color(0)]], texture2d<float, access::write> tex [[texture(3)]]);
// returns void; writes d0 to tex at ushort2(texcoord0_0)). The vertex shader is ours (outputs `color` and `texcoord0_0` = target pixel coordinates).
// Sequence: clear, plain quad, invert quad (fetch), COPY draw (whole target -> storage texture), then a plain quad AFTER the copy: the storage
// texture must hold the state at the copy, and the render target must be unchanged by the copy draw itself (void fragment: nothing written).
static int cmd_fbcopy(uint64_t rid, int haveRid, const char *out) {
    id<MTLDevice> dev = pick_device(rid, haveRid); if (!dev) return 2;
    NSError *err = nil;
    id<MTLLibrary> lib = n_lib(dev); if (!lib) return 2;
    id<MTLLibrary> qc = [dev newLibraryWithURL:[NSURL fileURLWithPath:@"/System/Library/Frameworks/QuartzCore.framework/Versions/A/Resources/default.metallib"] error:&err];
    perr("newLibraryWithURL(QuartzCore)", err);
    id<MTLFunction> vsq = [lib newFunctionWithName:@"blend_vs"], fsp = [lib newFunctionWithName:@"blend_fs"], fsf = [lib newFunctionWithName:@"fb_fs"], vsc = [lib newFunctionWithName:@"fbcopy_vs"];
    id<MTLFunction> fcp = qc ? [qc newFunctionWithName:@"inplace_copy_lpf"] : nil;
    if (!vsq || !fsp || !fsf || !vsc || !fcp) { printf("mtlprobe: FAIL functions\n"); return 2; }
    id<MTLRenderPipelineState> plain = fb_pso(dev, vsq, fsp, nil, "plain"), fetch = fb_pso(dev, vsq, fsf, nil, "fetch"), copy = fb_pso(dev, vsc, fcp, nil, "inplace_copy_lpf");
    if (!plain || !fetch || !copy) { printf("mtlprobe: FAIL pipeline\n"); return 2; }
    id<MTLTexture> tgt = n_tex(dev, MTLPixelFormatBGRA8Unorm, W, H, MTLTextureUsageRenderTarget, MTLStorageModePrivate);
    id<MTLTexture> cpy = n_tex(dev, MTLPixelFormatRGBA8Unorm, W, H, MTLTextureUsageShaderWrite | MTLTextureUsageShaderRead, MTLStorageModePrivate);
    if (!tgt || !cpy) { printf("mtlprobe: FAIL alloc\n"); return 2; }
    static const float INV_K[4] = { -1, -1, -1, 1 }, INV_C[4] = { 1, 1, 1, 0 }, colA[4] = { 200 / 255.0f, 120 / 255.0f, 30 / 255.0f, 1 }, colD[4] = { 10 / 255.0f, 250 / 255.0f, 90 / 255.0f, 1 };
    id<MTLCommandQueue> q = [dev newCommandQueue]; id<MTLCommandBuffer> cb = [q commandBuffer];
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = tgt; rp.colorAttachments[0].loadAction = MTLLoadActionClear; rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    rp.colorAttachments[0].clearColor = MTLClearColorMake(20.0 / 255, 40.0 / 255, 60.0 / 255, 1.0);
    id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:rp];
    float r[4];
    fb_rect(r, 40, 40, 168, 168, 256); [re setRenderPipelineState:plain]; [re setVertexBytes:r length:16 atIndex:0]; [re setFragmentBytes:colA length:16 atIndex:0];
    [re drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
    fb_rect(r, 88, 88, 216, 216, 256); [re setRenderPipelineState:fetch]; [re setVertexBytes:r length:16 atIndex:0]; [re setFragmentBytes:INV_K length:16 atIndex:0]; [re setFragmentBytes:INV_C length:16 atIndex:1];
    [re drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
    fb_rect(r, 0, 0, 256, 256, 256); [re setRenderPipelineState:copy]; [re setVertexBytes:r length:16 atIndex:0]; [re setFragmentTexture:cpy atIndex:3];
    [re drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
    fb_rect(r, 120, 20, 200, 120, 256); [re setRenderPipelineState:plain]; [re setVertexBytes:r length:16 atIndex:0]; [re setFragmentBytes:colD length:16 atIndex:0];
    [re drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
    [re endEncoding];
    if (!n_run(cb, "fbcopy")) { printf("mtlprobe: FAIL fbcopy (command buffer)\n"); return 2; }
    NSData *gt = n_readback(dev, q, tgt, W, H, 4), *gc = n_readback(dev, q, cpy, W, H, 4);
    if (!gt || !gc) { printf("mtlprobe: FAIL readback\n"); return 2; }
    static uint8_t exp[W * H * 4], expc[W * H * 4], gcb[W * H * 4];
    for (int i = 0; i < W * H; i++) { exp[i*4] = 60; exp[i*4+1] = 40; exp[i*4+2] = 20; exp[i*4+3] = 255; }
    fb_cpu_fill(exp, W, 40, 40, 168, 168, colA); fb_cpu_affine(exp, W, 88, 88, 216, 216, INV_K, INV_C);
    for (int i = 0; i < W * H; i++) { expc[i*4] = exp[i*4+2]; expc[i*4+1] = exp[i*4+1]; expc[i*4+2] = exp[i*4]; expc[i*4+3] = exp[i*4+3]; }   // RGBA storage texture <- BGRA state at the copy
    fb_cpu_fill(exp, W, 120, 20, 200, 120, colD);
    { const uint8_t *g = gc.bytes; for (int i = 0; i < W * H; i++) { gcb[i*4] = g[i*4+2]; gcb[i*4+1] = g[i*4+1]; gcb[i*4+2] = g[i*4]; gcb[i*4+3] = g[i*4+3]; } }   // compare in BGRA
    { uint8_t t[W * H * 4]; for (int i = 0; i < W * H; i++) { t[i*4] = expc[i*4+2]; t[i*4+1] = expc[i*4+1]; t[i*4+2] = expc[i*4]; t[i*4+3] = expc[i*4+3]; } memcpy(expc, t, sizeof t); }
    int dt = n_report_diff("fbcopy render target (tolerance 0)", gt.bytes, exp, W * H, 0, out, W, H);
    int dc = n_report_diff("fbcopy storage texture = state at the copy (tolerance 0)", gcb, expc, W * H, 0, NULL, W, H);
    printf(dt == 0 && dc == 0 ? "mtlprobe: PASS fbcopy (exact)\n" : "mtlprobe: FAIL fbcopy\n");
    return dt == 0 && dc == 0 ? 0 : 1;
}


// rndprobe (11e-2, diagnostic): how a plain (non-fetch) fragment output v/255 (v in 8-bit units) is quantised into BGRA8Unorm: prints the stored byte for several v.
static int cmd_rndprobe(uint64_t rid, int haveRid) {
    id<MTLDevice> dev = pick_device(rid, haveRid); if (!dev) return 2;
    id<MTLLibrary> lib = n_lib(dev); if (!lib) return 2;
    id<MTLFunction> vs = [lib newFunctionWithName:@"blend_vs"], fsp = [lib newFunctionWithName:@"blend_fs"];
    id<MTLRenderPipelineState> plain = fb_pso(dev, vs, fsp, nil, "plain"); if (!plain) return 2;
    id<MTLTexture> tgt = n_tex(dev, MTLPixelFormatBGRA8Unorm, W, H, MTLTextureUsageRenderTarget, MTLStorageModePrivate); if (!tgt) return 2;
    static const double vals[16] = { 80.55, 80.52, 80.51, 80.49, 80.45, 27.05, 27.45, 27.55, 100.30, 100.70, 200.55, 30.55, 150.5, 151.5, 0.4, 254.6 };
    id<MTLCommandQueue> q = [dev newCommandQueue]; id<MTLCommandBuffer> cb = [q commandBuffer];
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = tgt; rp.colorAttachments[0].loadAction = MTLLoadActionClear; rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
    id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:rp];
    [re setRenderPipelineState:plain];
    for (int i = 0; i < 16; i++) {
        float r[4]; fb_rect(r, (i % 4) * 64, (i / 4) * 64, (i % 4) * 64 + 64, (i / 4) * 64 + 64, 256);
        float c = (float)(vals[i] / 255.0), col[4] = { c, c, c, 1 };
        [re setVertexBytes:r length:16 atIndex:0]; [re setFragmentBytes:col length:16 atIndex:0];
        [re drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
    }
    [re endEncoding];
    if (!n_run(cb, "rndprobe")) return 2;
    NSData *got = n_readback(dev, q, tgt, W, H, 4); if (!got) return 2;
    const uint8_t *g = got.bytes;
    for (int i = 0; i < 16; i++) { const uint8_t *p = g + (((i / 4) * 64 + 32) * W + (i % 4) * 64 + 32) * 4; printf("mtlprobe: rndprobe: output %.2f/255 -> stored %u (round-to-nearest gives %ld)\n", vals[i], p[0], lround(vals[i])); }
    return 0;
}


// fbwithin (11e-2, informational): ONE draw of two overlapping quads (a triangle list) whose fragments invert the framebuffer through [[color(0)]].
// Metal (tile memory, primitive order) inverts the overlap twice; without rasterization-order attachment access Vulkan gives no such order inside one draw.
static int cmd_fbwithin(uint64_t rid, int haveRid, const char *out) {
    id<MTLDevice> dev = pick_device(rid, haveRid); if (!dev) return 2;
    id<MTLLibrary> lib = n_lib(dev); if (!lib) return 2;
    id<MTLFunction> vs = [lib newFunctionWithName:@"fbmulti_vs"], fsf = [lib newFunctionWithName:@"fb_fs"];
    if (!vs || !fsf) { printf("mtlprobe: FAIL functions\n"); return 2; }
    id<MTLRenderPipelineState> fetch = fb_pso(dev, vs, fsf, nil, "fetch"); if (!fetch) return 2;
    id<MTLTexture> tgt = n_tex(dev, MTLPixelFormatBGRA8Unorm, W, H, MTLTextureUsageRenderTarget, MTLStorageModePrivate); if (!tgt) return 2;
    static const float INV_K[4] = { -1, -1, -1, 1 }, INV_C[4] = { 1, 1, 1, 0 };
    float rects[8]; fb_rect(rects, 40, 40, 168, 168, 256); fb_rect(rects + 4, 100, 100, 228, 228, 256);
    id<MTLCommandQueue> q = [dev newCommandQueue]; id<MTLCommandBuffer> cb = [q commandBuffer];
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = tgt; rp.colorAttachments[0].loadAction = MTLLoadActionClear; rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    rp.colorAttachments[0].clearColor = MTLClearColorMake(20.0 / 255, 40.0 / 255, 60.0 / 255, 1.0);
    id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:rp];
    [re setRenderPipelineState:fetch]; [re setVertexBytes:rects length:sizeof rects atIndex:0]; [re setFragmentBytes:INV_K length:16 atIndex:0]; [re setFragmentBytes:INV_C length:16 atIndex:1];
    [re drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:12];
    [re endEncoding];
    if (!n_run(cb, "fbwithin")) { printf("mtlprobe: FAIL fbwithin (command buffer)\n"); return 2; }
    NSData *got = n_readback(dev, q, tgt, W, H, 4); if (!got) return 2;
    const uint8_t *g = got.bytes; int ovl = 0, ovOrig = 0, ovInv = 0, ovOther = 0, only = 0, onlyOk = 0;
    for (int y = 40; y < 228; y++) for (int x = 40; x < 228; x++) {
        int in1 = x >= 40 && x < 168 && y >= 40 && y < 168, in2 = x >= 100 && x < 228 && y >= 100 && y < 228; const uint8_t *p = g + (y * W + x) * 4;
        if (in1 && in2) { ovl++; if (p[0] == 60 && p[1] == 40 && p[2] == 20) ovOrig++; else if (p[0] == 195 && p[1] == 215 && p[2] == 235) ovInv++; else ovOther++; }
        else if (in1 || in2) { only++; if (p[0] == 195 && p[1] == 215 && p[2] == 235) onlyOk++; }
    }
    printf("mtlprobe: fbwithin: single-coverage pixels inverted once: %d of %d\n", onlyOk, only);
    printf("mtlprobe: fbwithin: overlap pixels: %d; inverted twice (Metal order, = original): %d; inverted once (no order inside the draw): %d; other: %d\n", ovl, ovOrig, ovInv, ovOther);
    if (out) { static uint8_t dummy; (void)dummy; write_png_wh(out, g, W, H); }
    return 0;
}

// ---------------------------------------------------------------------------------------------------------------
// #11 step 11h.6 (m11h6): IOSurface-backed textures. `mtlprobe iosurface`: (i) sample an IOSurface texture into a normal render target (CPU-written
// before commit; twice, the second time after a CPU rewrite), (ii) render into a 256x256 IOSurface render target (clear + 2 blended quads) and read the
// IOSurface on the CPU, (iii) a 2560x1440 IOSurface render target (clear + one quad), CPU checked, per-frame time over 100 frames.
// The CPU references were verified on the host Mac's real Metal first (arm64 build: same command, 0 differ).
// ---------------------------------------------------------------------------------------------------------------
static IOSurfaceRef io_make(int w, int h) {
    int32_t v[4] = { w, h, 4, 'BGRA' };
    CFNumberRef nw = CFNumberCreate(NULL, kCFNumberSInt32Type, &v[0]), nh = CFNumberCreate(NULL, kCFNumberSInt32Type, &v[1]),
                nb = CFNumberCreate(NULL, kCFNumberSInt32Type, &v[2]), np = CFNumberCreate(NULL, kCFNumberSInt32Type, &v[3]);
    CFMutableDictionaryRef pr = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(pr, kIOSurfaceWidth, nw); CFDictionarySetValue(pr, kIOSurfaceHeight, nh);
    CFDictionarySetValue(pr, kIOSurfaceBytesPerElement, nb); CFDictionarySetValue(pr, kIOSurfacePixelFormat, np);
    IOSurfaceRef s = IOSurfaceCreate(pr);
    CFRelease(pr); CFRelease(nw); CFRelease(nh); CFRelease(nb); CFRelease(np);
    return s;
}
static id<MTLTexture> io_tex(id<MTLDevice> dev, IOSurfaceRef s, int w, int h, MTLTextureUsage u) {
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:w height:h mipmapped:NO];
    td.usage = u; td.storageMode = N_MANAGED;
    return [dev newTextureWithDescriptor:td iosurface:s plane:0];
}
// CPU pattern (BGRA bytes): B = (3x+5y+seed)&255, G = y&255, R = x&255, A = 255 - ((x^y)&127).
static void io_fill(IOSurfaceRef s, int w, int h, int seed) {
    IOSurfaceLock(s, 0, NULL); uint8_t *b = IOSurfaceGetBaseAddress(s); size_t bpr = IOSurfaceGetBytesPerRow(s);
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) { uint8_t *p = b + y * bpr + x * 4;
        p[0] = (uint8_t)(3 * x + 5 * y + seed); p[1] = (uint8_t)y; p[2] = (uint8_t)x; p[3] = (uint8_t)(255 - ((x ^ y) & 127)); }
    IOSurfaceUnlock(s, 0, NULL);
}
// Copies the surface's rows into a tight w*h*4 array (locked read).
static NSData *io_read(IOSurfaceRef s, int w, int h) {
    NSMutableData *d = [NSMutableData dataWithLength:(NSUInteger)w * h * 4];
    IOSurfaceLock(s, kIOSurfaceLockReadOnly, NULL); const uint8_t *b = IOSurfaceGetBaseAddress(s); size_t bpr = IOSurfaceGetBytesPerRow(s);
    for (int y = 0; y < h; y++) memcpy((uint8_t *)d.mutableBytes + (size_t)y * w * 4, b + y * bpr, (size_t)w * 4);
    IOSurfaceUnlock(s, kIOSurfaceLockReadOnly, NULL);
    return d;
}
static void io_expect_blend(uint8_t *exp, int w, int h, const uint8_t clear[4] /*BGRA*/, int nq, const int (*qr)[4], const float (*qc)[4]) {
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
        uint8_t q8[4] = { clear[0], clear[1], clear[2], clear[3] };
        for (int k = 0; k < nq; k++) if (x >= qr[k][0] && x < qr[k][2] && y >= qr[k][1] && y < qr[k][3]) {
            const float *c = qc[k]; float a = c[3]; float s[4] = { c[2] * a, c[1] * a, c[0] * a, a }, d[4];
            for (int m = 0; m < 4; m++) d[m] = s[m] + (q8[m] / 255.0f) * (1.0f - a);
            for (int m = 0; m < 4; m++) { float v = d[m] < 0 ? 0 : d[m] > 1 ? 1 : d[m]; q8[m] = (uint8_t)lrintf(v * 255.0f); }
        }
        memcpy(exp + ((size_t)y * w + x) * 4, q8, 4);
    }
}

// ---- S5.2a: dispflip (native #12, an internal design note). A 2560x1440 BGRA IOSurface render target with moving bands, rendered on the Navi48 device; the bundle's D-copy
// identifies it as a display surface (N48M_TEST_DISPFLIP=1 waives the CoreDisplay backtrace signal; root + N48M_ALLOW=1 only), copies it into a scanout slot after each command buffer
// and presents. Reports latched/s from the kernel status (via the bundle's n48ScanoutStats selector: the N48N client is exclusive, mtlprobe cannot open a second one).
// On any other device (an Apple-silicon Mac's AGX) the selector is absent and the command REFUSES cleanly (exit 3). Needs the PC with WindowServer NOT holding N48N: see NATIVE-S5-FLIP.md "S5.2a build".
static uint64_t df_now(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }
static double df_num(NSDictionary *d, NSString *k) { return [d[k] doubleValue]; }
static int cmd_dispflip(uint64_t rid, int haveRid, int seconds) {
    setenv("N48M_TEST_DISPFLIP", "1", 1);   // read by the bundle (honoured only as root with N48M_ALLOW=1)
    id<MTLDevice> dev = pick_device(rid, haveRid); if (!dev) return 2;
    if (![(NSObject *)dev respondsToSelector:NSSelectorFromString(@"n48ScanoutStats")]) {
        printf("mtlprobe: dispflip: REFUSED (clean): device '%s' has no scanout path (no n48ScanoutStats: not the Navi48Metal bundle; scanout is native-kext-only, the bundle's own query answers -ENOSYS elsewhere)\n", [dev.name UTF8String]);
        return 3;
    }
    NSDictionary *(*stats)(id, SEL) = (NSDictionary *(*)(id, SEL))objc_msgSend;
    NSDictionary *(*rel)(id, SEL) = (NSDictionary *(*)(id, SEL))objc_msgSend;
    const int DW = 2560, DH = 1440;
    id<MTLLibrary> lib = n_lib(dev); if (!lib) return 2;
    id<MTLFunction> vs = [lib newFunctionWithName:@"blend_vs"], fs = [lib newFunctionWithName:@"blend_fs"];
    if (!vs || !fs) { printf("mtlprobe: FAIL functions\n"); return 2; }
    MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
    pd.vertexFunction = vs; pd.fragmentFunction = fs; pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    NSError *err = nil; id<MTLRenderPipelineState> pso = [dev newRenderPipelineStateWithDescriptor:pd error:&err]; perr("dispflip pso", err);
    IOSurfaceRef surf = io_make(DW, DH);
    if (!pso || !surf) { printf("mtlprobe: FAIL pipeline/IOSurfaceCreate\n"); return 2; }
    printf("mtlprobe: dispflip: IOSurface id %u %zux%zu bytesPerRow %zu allocSize %zu (display plane: pitch 10240)\n", IOSurfaceGetID(surf), IOSurfaceGetWidth(surf), IOSurfaceGetHeight(surf), IOSurfaceGetBytesPerRow(surf), IOSurfaceGetAllocSize(surf));
    id<MTLTexture> tex = io_tex(dev, surf, DW, DH, MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead);
    id<MTLCommandQueue> q = [dev newCommandQueue];
    if (!tex || !q) { printf("mtlprobe: FAIL texture/queue\n"); return 2; }
    dispatch_semaphore_t sem = dispatch_semaphore_create(3);
    __block int cbErrors = 0;
    const int NB = 12; uint64_t t0 = df_now(), tPrev = t0; double lastLatched = 0; int frame = 0, lastFrame = 0, fails = 0; int refused = 0;
    NSDictionary *st = nil; double minRate = 1e9, maxRate = 0, sumRate = 0; int nrate = 0;
    while (df_now() - t0 < (uint64_t)seconds * 1000000000ull) {
        dispatch_semaphore_wait(sem, DISPATCH_TIME_FOREVER);
        id<MTLCommandBuffer> cb = [q commandBuffer];
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = tex; rp.colorAttachments[0].loadAction = MTLLoadActionClear; rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        rp.colorAttachments[0].clearColor = MTLClearColorMake(0.05, 0.05, 0.10, 1.0);
        id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:rp];
        [re setRenderPipelineState:pso];
        int bw = DW / NB, off = (frame * 16) % DW;   // 16 px per frame: a band crosses the screen in 160 frames (2.7 s at 60/s)
        for (int k = 0; k < NB; k++) {
            float hue = (float)k / NB; float c[4] = { 0.5f + 0.5f * sinf(6.2831853f * hue), 0.5f + 0.5f * sinf(6.2831853f * (hue + 0.333f)), 0.5f + 0.5f * sinf(6.2831853f * (hue + 0.667f)), 1.0f };
            for (int wrap = 0; wrap < 2; wrap++) {
                int x0 = (k * bw + off) % DW - (wrap ? DW : 0); int x1 = x0 + bw / 2;   // bands half as wide as their pitch; the wrapped copy covers the right edge
                if (x1 <= 0 || x0 >= DW) continue;
                int cx0 = x0 < 0 ? 0 : x0, cx1 = x1 > DW ? DW : x1;
                float rect[4] = { cx0 / (DW / 2.0f) - 1.0f, -1.0f, (cx1 - cx0) / (DW / 2.0f), 2.0f };
                [re setVertexBytes:rect length:sizeof rect atIndex:0]; [re setFragmentBytes:c length:sizeof c atIndex:0];
                [re drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
            }
        }
        [re endEncoding];
        [cb addCompletedHandler:^(id<MTLCommandBuffer> c) { if (c.status != MTLCommandBufferStatusCompleted || c.error) cbErrors++; dispatch_semaphore_signal(sem); }];
        [cb commit]; frame++;
        if (frame == 1) {   // the first command buffer decides: is a display surface identified and the plane acquired?
            [cb waitUntilCompleted];
            st = stats(dev, NSSelectorFromString(@"n48ScanoutStats"));
            printf("mtlprobe: dispflip: after frame 1: state %s reason %s presents %.0f drops %.0f display surfaces %.0f\n", [st[@"state"] UTF8String], [st[@"reason"] UTF8String], df_num(st, @"presents"), df_num(st, @"drops"), df_num(st, @"disp_surfaces"));
            if (![st[@"state"] isEqualToString:@"active"]) { printf("mtlprobe: dispflip: REFUSED (clean): D-copy is %s (%s); nothing was presented (see the bundle's 'scanout:' log lines on stderr)\n", [st[@"state"] UTF8String], [st[@"reason"] UTF8String]); refused = 1; break; }
        }
        uint64_t now = df_now();
        if (now - tPrev >= 1000000000ull) {
            st = stats(dev, NSSelectorFromString(@"n48ScanoutStats"));
            double lat = df_num(st, @"latched"), dt = (now - tPrev) / 1e9, rate = (lat - lastLatched) / dt;
            printf("mtlprobe: dispflip: t=%2.0fs frames %d (+%d) presents %.0f drops %.0f gpu_fail %.0f | kernel latched %.0f (%.1f/s) replaced %.0f repeats %.0f refused %.0f watchdog_restores %.0f\n", (now - t0) / 1e9, frame, frame - lastFrame, df_num(st, @"presents"), df_num(st, @"drops"),
                   df_num(st, @"gpu_fail"), lat, rate, df_num(st, @"replaced"), df_num(st, @"repeats"), df_num(st, @"refused"), df_num(st, @"watchdog_restores"));
            if (nrate > 0 || (now - t0) > 2000000000ull) { if (rate < minRate) minRate = rate; if (rate > maxRate) maxRate = rate; sumRate += rate; nrate++; }
            lastLatched = lat; tPrev = now; lastFrame = frame;
        }
    }
    for (int i = 0; i < 3; i++) dispatch_semaphore_wait(sem, DISPATCH_TIME_FOREVER);   // drain: every command buffer completed (its slot was presented or counted)
    if (refused) { NSDictionary *r = rel(dev, NSSelectorFromString(@"n48ScanoutRelease")); (void)r; return 3; }
    st = stats(dev, NSSelectorFromString(@"n48ScanoutStats"));
    double secs = (df_now() - t0) / 1e9;
    printf("mtlprobe: dispflip: total %.1f s, %d frames (%.1f/s), command buffer errors %d | presents %.0f drops %.0f gpu_fail %.0f present_fail %.0f | kernel: latched %.0f replaced %.0f repeats %.0f refused %.0f watchdog_restores %.0f vupdates %.0f\n",
           secs, frame, frame / secs, cbErrors, df_num(st, @"presents"), df_num(st, @"drops"), df_num(st, @"gpu_fail"), df_num(st, @"present_fail"), df_num(st, @"latched"), df_num(st, @"replaced"), df_num(st, @"repeats"), df_num(st, @"refused"), df_num(st, @"watchdog_restores"), df_num(st, @"vupdates"));
    double avg = nrate ? sumRate / nrate : 0;
    NSDictionary *r = rel(dev, NSSelectorFromString(@"n48ScanoutRelease"));
    int relOK = [r[@"rc"] intValue] == 0 && [r[@"verified"] intValue] == 1;
    printf("mtlprobe: dispflip: latched/s avg %.1f min %.1f max %.1f over %d samples (S5.2b pass line: avg >= 59)\n", avg, nrate ? minRate : 0, maxRate, nrate);
    printf("mtlprobe: dispflip: release rc %d, console restore verified %d, plane MC after 0x%llx\n", [r[@"rc"] intValue], [r[@"verified"] intValue], (unsigned long long)[r[@"plane_mc"] unsignedLongLongValue]);
    if (cbErrors) fails++;
    if (df_num(st, @"replaced") != 0 || df_num(st, @"refused") != 0 || df_num(st, @"watchdog_restores") != 0 || df_num(st, @"present_fail") != 0) fails++;
    if (avg < 59.0) fails++;
    if (!relOK) fails++;
    printf(fails ? "mtlprobe: FAIL dispflip (%d criteria; see lines above)\n" : "mtlprobe: PASS dispflip\n", fails);
    return fails ? 1 : 0;
}

static int cmd_iosurface(uint64_t rid, int haveRid) {
    id<MTLDevice> dev = pick_device(rid, haveRid); if (!dev) return 2;
    id<MTLLibrary> lib = n_lib(dev); if (!lib) return 2;
    id<MTLCommandQueue> q = [dev newCommandQueue];
    int fails = 0;
    // ---- pipelines ----
    id<MTLFunction> qvs = [lib newFunctionWithName:@"quad_vs"], qfs = [lib newFunctionWithName:@"quad_fs"];
    id<MTLFunction> bvs = [lib newFunctionWithName:@"blend_vs"], bfs = [lib newFunctionWithName:@"blend_fs"];
    if (!qvs || !qfs || !bvs || !bfs) { printf("mtlprobe: FAIL functions\n"); return 2; }
    MTLVertexDescriptor *vd = [MTLVertexDescriptor vertexDescriptor];
    vd.attributes[0].format = MTLVertexFormatFloat2; vd.attributes[0].offset = 0; vd.attributes[0].bufferIndex = 0;
    vd.attributes[1].format = MTLVertexFormatFloat2; vd.attributes[1].offset = 8; vd.attributes[1].bufferIndex = 0;
    vd.layouts[0].stride = 16; vd.layouts[0].stepFunction = MTLVertexStepFunctionPerVertex; vd.layouts[0].stepRate = 1;
    MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
    pd.vertexFunction = qvs; pd.fragmentFunction = qfs; pd.vertexDescriptor = vd; pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    NSError *err = nil; id<MTLRenderPipelineState> qpso = [dev newRenderPipelineStateWithDescriptor:pd error:&err]; perr("quad pso", err);
    MTLRenderPipelineDescriptor *bd = [MTLRenderPipelineDescriptor new];
    bd.vertexFunction = bvs; bd.fragmentFunction = bfs;
    MTLRenderPipelineColorAttachmentDescriptor *ca = bd.colorAttachments[0];
    ca.pixelFormat = MTLPixelFormatBGRA8Unorm; ca.blendingEnabled = YES; ca.rgbBlendOperation = MTLBlendOperationAdd; ca.alphaBlendOperation = MTLBlendOperationAdd;
    ca.sourceRGBBlendFactor = MTLBlendFactorOne; ca.sourceAlphaBlendFactor = MTLBlendFactorOne;
    ca.destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha; ca.destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
    err = nil; id<MTLRenderPipelineState> bpso = [dev newRenderPipelineStateWithDescriptor:bd error:&err]; perr("blend pso", err);
    MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new];
    sd.minFilter = MTLSamplerMinMagFilterNearest; sd.magFilter = MTLSamplerMinMagFilterNearest; sd.sAddressMode = MTLSamplerAddressModeClampToEdge; sd.tAddressMode = MTLSamplerAddressModeClampToEdge;
    id<MTLSamplerState> ss = [dev newSamplerStateWithDescriptor:sd];
    float verts[16] = { -1, 1, 0, 0,   1, 1, 1, 0,   -1, -1, 0, 1,   1, -1, 1, 1 }; uint16_t idx[6] = { 0, 1, 2, 2, 1, 3 };
    id<MTLBuffer> vb = [dev newBufferWithLength:sizeof verts options:MTLResourceStorageModeShared], ib = [dev newBufferWithLength:sizeof idx options:MTLResourceStorageModeShared];
    if (!qpso || !bpso || !ss || !vb || !ib) { printf("mtlprobe: FAIL pipelines/alloc\n"); return 2; }
    memcpy(vb.contents, verts, sizeof verts); memcpy(ib.contents, idx, sizeof idx);

    // ---- (i) IOSurface as a sampled texture ----
    printf("mtlprobe: iosurface (i): sample a 256x256 BGRA IOSurface into a normal render target\n");
    IOSurfaceRef sa = io_make(W, H);
    if (!sa) { printf("mtlprobe: FAIL IOSurfaceCreate\n"); return 2; }
    printf("mtlprobe: iosurface (i): surface id %u %zux%zu bytesPerRow %zu allocSize %zu\n", IOSurfaceGetID(sa), IOSurfaceGetWidth(sa), IOSurfaceGetHeight(sa), IOSurfaceGetBytesPerRow(sa), IOSurfaceGetAllocSize(sa));
    io_fill(sa, W, H, 0);
    id<MTLTexture> ta = io_tex(dev, sa, W, H, MTLTextureUsageShaderRead);
    printf("mtlprobe: iosurface (i): newTextureWithDescriptor:iosurface:plane: %s; texture.iosurface == surface: %s\n", ta ? "non-nil" : "NIL", (ta && ta.iosurface == sa) ? "yes" : "NO");
    id<MTLTexture> tgt = n_tex(dev, MTLPixelFormatBGRA8Unorm, W, H, MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead, MTLStorageModePrivate);
    if (!ta || !tgt) { printf("mtlprobe: FAIL iosurface (i) alloc\n"); return 2; }
    static uint8_t exp[2560 * 1440 * 4];
    for (int round = 0; round < 3; round++) {
        if (round == 1) io_fill(sa, W, H, 77);    // CPU rewrite AFTER the texture exists and was used once: the next command buffer must see it
        if (round == 2) { IOSurfaceLock(sa, 0, NULL); uint8_t *b = IOSurfaceGetBaseAddress(sa); size_t bpr = IOSurfaceGetBytesPerRow(sa);   // partial CPU rewrite: one 32x32 block
            for (int y = 100; y < 132; y++) for (int x = 60; x < 92; x++) { uint8_t *p = b + y * bpr + x * 4; p[0] = 1; p[1] = 2; p[2] = 3; p[3] = 4; }
            IOSurfaceUnlock(sa, 0, NULL); }
        id<MTLCommandBuffer> cb = [q commandBuffer];
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = tgt; rp.colorAttachments[0].loadAction = MTLLoadActionClear; rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);
        id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:rp];
        float xf[4] = { 1, 1, 0, 0 }, tint[4] = { 1, 1, 1, 1 };
        [re setRenderPipelineState:qpso]; [re setViewport:(MTLViewport){ 0, 0, W, H, 0, 1 }]; [re setScissorRect:(MTLScissorRect){ 0, 0, W, H }];
        [re setVertexBuffer:vb offset:0 atIndex:0]; [re setVertexBytes:xf length:sizeof xf atIndex:1]; [re setFragmentBytes:tint length:sizeof tint atIndex:0];
        [re setFragmentTexture:ta atIndex:0]; [re setFragmentSamplerState:ss atIndex:0];
        [re drawIndexedPrimitives:MTLPrimitiveTypeTriangle indexCount:6 indexType:MTLIndexTypeUInt16 indexBuffer:ib indexBufferOffset:0];
        [re endEncoding];
        if (!n_run(cb, "iosurface-sample")) { printf("mtlprobe: FAIL iosurface (i) command buffer\n"); return 2; }
        NSData *got = n_readback(dev, q, tgt, W, H, 4); if (!got) { printf("mtlprobe: FAIL iosurface (i) readback\n"); return 2; }
        NSData *cpu = io_read(sa, W, H);   // the CPU's view of the surface is the reference (the GPU must reproduce exactly what the CPU wrote)
        char tag[64]; snprintf(tag, sizeof tag, "iosurface (i) round %d (%s)", round, round == 0 ? "first use" : round == 1 ? "after a full CPU rewrite" : "after a 32x32 CPU rewrite");
        int diff = n_report_diff(tag, got.bytes, cpu.bytes, W * H, 0, NULL, W, H);
        if (diff) fails++;
    }
    printf(fails == 0 ? "mtlprobe: PASS iosurface (i) sample (0 differ, 3 rounds incl. CPU rewrites)\n" : "mtlprobe: FAIL iosurface (i)\n");

    // ---- (ii) IOSurface as a render target (256x256, clear + 2 blended quads) ----
    printf("mtlprobe: iosurface (ii): render into a 256x256 IOSurface (clear + 2 blended quads), CPU check\n");
    IOSurfaceRef sb = io_make(W, H);
    id<MTLTexture> tb = sb ? io_tex(dev, sb, W, H, MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead) : nil;
    if (!tb) { printf("mtlprobe: FAIL iosurface (ii) alloc\n"); return 2; }
    memset(IOSurfaceGetBaseAddress(sb), 0xEE, IOSurfaceGetAllocSize(sb));   // poison: only a correct write-back can replace it
    int fails2 = 0;
    { const int qr[2][4] = { { 40, 40, 168, 168 }, { 88, 88, 216, 216 } }; const float qc[2][4] = { { 0.8f, 0.2f, 0.1f, 0.5f }, { 0.1f, 0.6f, 0.9f, 0.25f } };
      const uint8_t clear[4] = { 60, 40, 20, 255 };
      id<MTLCommandBuffer> cb = [q commandBuffer];
      MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
      rp.colorAttachments[0].texture = tb; rp.colorAttachments[0].loadAction = MTLLoadActionClear; rp.colorAttachments[0].storeAction = MTLStoreActionStore;
      rp.colorAttachments[0].clearColor = MTLClearColorMake(20.0 / 255, 40.0 / 255, 60.0 / 255, 1.0);
      id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:rp];
      [re setRenderPipelineState:bpso];
      for (int k = 0; k < 2; k++) {
          float rect[4] = { qr[k][0] / 128.0f - 1.0f, 1.0f - qr[k][3] / 128.0f, (qr[k][2] - qr[k][0]) / 128.0f, (qr[k][3] - qr[k][1]) / 128.0f };
          [re setVertexBytes:rect length:sizeof rect atIndex:0]; [re setFragmentBytes:qc[k] length:sizeof qc[k] atIndex:0];
          [re drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
      }
      [re endEncoding];
      if (!n_run(cb, "iosurface-render")) { printf("mtlprobe: FAIL iosurface (ii) command buffer\n"); return 2; }
      NSData *got = io_read(sb, W, H);
      io_expect_blend(exp, W, H, clear, 2, qr, qc);
      int d1 = n_report_diff("iosurface (ii) render target vs CPU reference (tolerance 1 LSB)", got.bytes, exp, W * H, 1, NULL, W, H);
      int d0 = n_report_diff("iosurface (ii) render target vs CPU reference (tolerance 0, informational)", got.bytes, exp, W * H, 0, NULL, W, H);
      printf("mtlprobe: iosurface (ii): %d exact-different, %d beyond 1 LSB\n", d0, d1);
      fails2 = d1; }
    // a second command buffer LOADs the IOSurface contents (CPU-poked pixel first) and blends one more quad over them: the render target's previous contents must be preserved
    { IOSurfaceLock(sb, 0, NULL); uint8_t *b = IOSurfaceGetBaseAddress(sb); size_t bpr = IOSurfaceGetBytesPerRow(sb);
      for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++) { uint8_t *p = b + y * bpr + x * 4; p[0] = 9; p[1] = 8; p[2] = 7; p[3] = 255; }
      IOSurfaceUnlock(sb, 0, NULL);
      memcpy(exp, (const uint8_t *)io_read(sb, W, H).bytes, (size_t)W * H * 4);   // reference = the CPU's current view, then the quad on top (CPU maths)
      const int qr[1][4] = { { 4, 4, 30, 30 } }; const float qc[1][4] = { { 0.5f, 0.5f, 0.5f, 0.5f } };
      for (int y = 4; y < 30; y++) for (int x = 4; x < 30; x++) { uint8_t *e = exp + ((size_t)y * W + x) * 4; float a = 0.5f;
          float s[4] = { 0.5f * a, 0.5f * a, 0.5f * a, a };
          for (int m = 0; m < 4; m++) { float v = s[m] + (e[m] / 255.0f) * (1 - a); e[m] = (uint8_t)lrintf((v > 1 ? 1 : v) * 255.0f); } }
      id<MTLCommandBuffer> cb = [q commandBuffer];
      MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
      rp.colorAttachments[0].texture = tb; rp.colorAttachments[0].loadAction = MTLLoadActionLoad; rp.colorAttachments[0].storeAction = MTLStoreActionStore;
      id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:rp];
      [re setRenderPipelineState:bpso];
      float rect[4] = { qr[0][0] / 128.0f - 1.0f, 1.0f - qr[0][3] / 128.0f, (qr[0][2] - qr[0][0]) / 128.0f, (qr[0][3] - qr[0][1]) / 128.0f };
      [re setVertexBytes:rect length:sizeof rect atIndex:0]; [re setFragmentBytes:qc[0] length:sizeof qc[0] atIndex:0];
      [re drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
      [re endEncoding];
      if (!n_run(cb, "iosurface-load")) { printf("mtlprobe: FAIL iosurface (ii) load command buffer\n"); return 2; }
      NSData *got = io_read(sb, W, H);
      int d1 = n_report_diff("iosurface (ii) load-action-Load pass over CPU-modified contents (tolerance 1 LSB)", got.bytes, exp, W * H, 1, NULL, W, H);
      fails2 += d1; }
    printf(fails2 == 0 ? "mtlprobe: PASS iosurface (ii) render target (0 differ vs CPU reference)\n" : "mtlprobe: FAIL iosurface (ii)\n");
    fails += fails2;

    // ---- (iii) display-size IOSurface render target ----
    const int DW = 2560, DH = 1440;
    printf("mtlprobe: iosurface (iii): %dx%d IOSurface render target, clear + one quad, 100 frames\n", DW, DH);
    IOSurfaceRef sc = io_make(DW, DH);
    id<MTLTexture> tc = sc ? io_tex(dev, sc, DW, DH, MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead) : nil;
    if (!tc) { printf("mtlprobe: FAIL iosurface (iii) alloc\n"); return 2; }
    printf("mtlprobe: iosurface (iii): surface id %u bytesPerRow %zu (tight %d) allocSize %zu\n", IOSurfaceGetID(sc), IOSurfaceGetBytesPerRow(sc), DW * 4, IOSurfaceGetAllocSize(sc));
    memset(IOSurfaceGetBaseAddress(sc), 0xEE, IOSurfaceGetAllocSize(sc));
    double tmin = 1e9, tmax = 0, tsum = 0; int fails3 = 0, checked = 0;
    for (int f = 0; f < 100; f++) {
        uint8_t cl[4] = { (uint8_t)(f * 2), (uint8_t)(100 + f), (uint8_t)(200 - f), 255 };   // B,G,R,A
        const int qr[1][4] = { { 100 + f * 5, 200, 1500 + f * 5, 900 } }; const float qc[1][4] = { { 0.9f, 0.4f, 0.2f, 1.0f } };
        double t0 = now_s();
        id<MTLCommandBuffer> cb = [q commandBuffer];
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = tc; rp.colorAttachments[0].loadAction = MTLLoadActionClear; rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        rp.colorAttachments[0].clearColor = MTLClearColorMake(cl[2] / 255.0, cl[1] / 255.0, cl[0] / 255.0, 1.0);
        id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:rp];
        [re setRenderPipelineState:bpso];
        float rect[4] = { qr[0][0] * 2.0f / DW - 1.0f, 1.0f - qr[0][3] * 2.0f / DH, (qr[0][2] - qr[0][0]) * 2.0f / DW, (qr[0][3] - qr[0][1]) * 2.0f / DH };
        [re setVertexBytes:rect length:sizeof rect atIndex:0]; [re setFragmentBytes:qc[0] length:sizeof qc[0] atIndex:0];
        [re drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
        [re endEncoding];
        [cb commit]; [cb waitUntilCompleted];
        double dt = now_s() - t0; tsum += dt; if (dt < tmin) tmin = dt; if (dt > tmax) tmax = dt;
        if (cb.status != MTLCommandBufferStatusCompleted || cb.error) { printf("mtlprobe: FAIL iosurface (iii) frame %d status %ld\n", f, (long)cb.status); perr("frame", cb.error); return 2; }
        if (f == 0 || f % 20 == 0 || f == 99) {
            NSData *got = io_read(sc, DW, DH); io_expect_blend(exp, DW, DH, cl, 1, qr, qc);
            char tag[64]; snprintf(tag, sizeof tag, "iosurface (iii) frame %d", f);
            int d1 = n_report_diff(tag, got.bytes, exp, DW * DH, 1, NULL, DW, DH); fails3 += d1; checked++;
        }
    }
    printf("mtlprobe: iosurface (iii): 100 frames (commit+waitUntilCompleted each): total %.3f s, per frame avg %.2f ms min %.2f max %.2f (%.1f frames/s); %d frames CPU-checked\n",
           tsum, tsum * 10.0, tmin * 1000, tmax * 1000, 100.0 / tsum, checked);
    printf(fails3 == 0 ? "mtlprobe: PASS iosurface (iii) display-size render target (0 differ vs CPU reference)\n" : "mtlprobe: FAIL iosurface (iii)\n");
    fails += fails3;
#if !defined(__arm64__)
    // ---- (iv) refusals (bundle behaviour: nil + log; real Metal asserts on some of these, so the host Mac does not run this) ----
    { int f4 = 0;
      id<MTLTexture> n1 = io_tex(dev, sa, 128, 128, MTLTextureUsageShaderRead);   // descriptor size != surface size
      printf("mtlprobe: iosurface (iv): descriptor 128x128 on a 256x256 surface -> %s\n", n1 ? "NON-NIL (unexpected)" : "nil (refused)"); if (n1) f4++;
      int32_t v8[3] = { 64, 64, 1 }; CFNumberRef n8w = CFNumberCreate(NULL, kCFNumberSInt32Type, &v8[0]), n8h = CFNumberCreate(NULL, kCFNumberSInt32Type, &v8[1]), n8b = CFNumberCreate(NULL, kCFNumberSInt32Type, &v8[2]);
      CFMutableDictionaryRef pr = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
      CFDictionarySetValue(pr, kIOSurfaceWidth, n8w); CFDictionarySetValue(pr, kIOSurfaceHeight, n8h); CFDictionarySetValue(pr, kIOSurfaceBytesPerElement, n8b);
      IOSurfaceRef s8 = IOSurfaceCreate(pr);
      id<MTLTexture> n2 = s8 ? io_tex(dev, s8, 64, 64, MTLTextureUsageShaderRead) : nil;
      printf("mtlprobe: iosurface (iv): 1 byte/element surface -> %s\n", n2 ? "NON-NIL (unexpected)" : "nil (refused)"); if (n2 || !s8) f4++;
      MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:W height:H mipmapped:NO]; td.usage = MTLTextureUsageShaderRead; td.storageMode = N_MANAGED;
      id<MTLTexture> n3 = [dev newTextureWithDescriptor:td iosurface:sa plane:1];
      printf("mtlprobe: iosurface (iv): plane 1 of a single-plane surface -> %s\n", n3 ? "NON-NIL (unexpected)" : "nil (refused)"); if (n3) f4++;
      td.pixelFormat = MTLPixelFormatRGBA16Float;
      id<MTLTexture> n4 = [dev newTextureWithDescriptor:td iosurface:sa plane:0];
      printf("mtlprobe: iosurface (iv): RGBA16Float descriptor on a 32 bpp surface -> %s\n", n4 ? "NON-NIL (unexpected)" : "nil (refused)"); if (n4) f4++;
      printf(f4 == 0 ? "mtlprobe: PASS iosurface (iv) refusals\n" : "mtlprobe: FAIL iosurface (iv)\n"); fails += f4; }
#endif
    printf(fails == 0 ? "mtlprobe: PASS iosurface (all)\n" : "mtlprobe: FAIL iosurface\n");
    return fails == 0 ? 0 : 1;
}

// ---- selcensus (#11 prep, Metal API gap census): for every selector of the public + SPI Metal protocols, does each live object of
// the bundle respond, and which class implements it (the bundle's N48*/Navi48* = ours; _MTL*/MTLIOAccel* = inherited Apple code)? ----
static const char *sc_owner(id obj, SEL sel, const char **img) {
    *img = "";
    for (Class c = object_getClass(obj); c; c = class_getSuperclass(c)) {
        unsigned n = 0; Method *ml = class_copyMethodList(c, &n); BOOL hit = NO;
        for (unsigned i = 0; i < n; i++) if (method_getName(ml[i]) == sel) { hit = YES; Dl_info di; IMP im = method_getImplementation(ml[i]);
            if (dladdr((void *)im, &di) && di.dli_fname) { const char *s = strrchr(di.dli_fname, '/'); *img = s ? s + 1 : di.dli_fname; } break; }
        free(ml);
        if (hit) return class_getName(c);
    }
    return NULL;
}
static void sc_proto(const char *oname, id obj, const char *pname) {
    Protocol *p = objc_getProtocol(pname);
    if (!p) { printf("SC %s %s: (protocol not present in this OS)\n", oname, pname); return; }
    for (int req = 1; req >= 0; req--) for (int inst = 1; inst >= 0; inst--) {
        if (!inst) continue;   // instance methods only (class methods of these protocols do not exist)
        unsigned n = 0; struct objc_method_description *md = protocol_copyMethodDescriptionList(p, req, YES, &n);
        for (unsigned i = 0; i < n; i++) {
            const char *img; const char *own = sc_owner(obj, md[i].name, &img);
            printf("SC\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n", oname, pname, req ? "req" : "opt", sel_getName(md[i].name), own ? own : "MISSING", img, md[i].types ? md[i].types : "");
        }
        free(md);
    }
}

// ---------------------------------------------------------------------------------------------------------------
// build 16 (app-fix round 1, P1 + F1): `mtlprobe ioalias`.
//  (1) Preview's "image disappears": in ONE command buffer a blit writes wrapper A of an IOSurface, a second blit reads wrapper B (a NEW texture object over the same surface) into a buffer, and the buffer must hold exactly
//      the pattern A wrote. Before build 16 a path (b) wrapper B uploaded the surface's stale pages (zero for a fresh surface). Run it natural and with N48M_IOS_PATH=b (forces the copy path).
//  (2) CLOBBER: A writes pattern 1, a second wrapper C of the same surface writes pattern 2 after it, same command buffer: the surface's pages must end as pattern 2 (the older image of A must not be copied over them).
//  (3) a re-read: B reads again after C wrote (B was touched before): must see pattern 2.
//  (4) F1 (only when N48M_TEST_IMPORT_FAIL_EVERY is set, root + N48M_ALLOW=1): newTexture(iosurface) must return a texture, not nil, when the import is refused (the content is blank, the process stays up).
// Apple's Metal passes (1)-(3) trivially (verified on the host Mac); on our driver (1)-(3) are the P1 check, run as root on the PC with N48M_ALLOW=1 and the process treated as an application (build 16: aliasing acts in applications only).
// ---------------------------------------------------------------------------------------------------------------
static void io_blit_write(id<MTLDevice> dev, id<MTLCommandBuffer> cb, id<MTLTexture> t, int w, int h, int seed) {
    id<MTLBuffer> src = [dev newBufferWithLength:(NSUInteger)w * h * 4 options:MTLResourceStorageModeShared]; uint8_t *p = src.contents;
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) { uint8_t *q = p + (y * w + x) * 4; q[0] = (uint8_t)(3 * x + 5 * y + seed); q[1] = (uint8_t)y; q[2] = (uint8_t)x; q[3] = (uint8_t)(255 - ((x ^ y) & 127)); }
    id<MTLBlitCommandEncoder> be = [cb blitCommandEncoder];
    [be copyFromBuffer:src sourceOffset:0 sourceBytesPerRow:(NSUInteger)w * 4 sourceBytesPerImage:(NSUInteger)w * h * 4 sourceSize:MTLSizeMake(w, h, 1) toTexture:t destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
    [be endEncoding];
}
static int io_blit_read_check(id<MTLDevice> dev, id<MTLCommandBuffer> cb, id<MTLTexture> t, int w, int h, id<MTLBuffer> __strong *outBuf) {
    id<MTLBuffer> dst = [dev newBufferWithLength:(NSUInteger)w * h * 4 options:MTLResourceStorageModeShared]; memset(dst.contents, 0xEE, (size_t)w * h * 4);
    id<MTLBlitCommandEncoder> be = [cb blitCommandEncoder];
    [be copyFromTexture:t sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(w, h, 1) toBuffer:dst destinationOffset:0 destinationBytesPerRow:(NSUInteger)w * 4 destinationBytesPerImage:(NSUInteger)w * h * 4];
    [be endEncoding]; *outBuf = dst; return 0;
}
static int io_buf_matches(id<MTLBuffer> b, int w, int h, int seed) {
    const uint8_t *p = b.contents; int bad = 0;
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) { const uint8_t *q = p + (y * w + x) * 4; uint8_t e[4] = { (uint8_t)(3 * x + 5 * y + seed), (uint8_t)y, (uint8_t)x, (uint8_t)(255 - ((x ^ y) & 127)) }; if (memcmp(q, e, 4)) bad++; }
    return bad;
}
static int cmd_ioalias(uint64_t rid, int haveRid) {
    id<MTLDevice> dev = pick_device(rid, haveRid); if (!dev) return 2;
    id<MTLCommandQueue> q = [dev newCommandQueue];
    const int IW = 64, IH = 64, S1 = 11, S2 = 77; int fails = 0;
    const char *force = getenv("N48M_IOS_PATH");
    printf("mtlprobe: ioalias: %dx%d BGRA IOSurface, N48M_IOS_PATH=%s\n", IW, IH, force ? force : "(natural)");
    // (1) write via A, read via B, one command buffer
    IOSurfaceRef s = io_make(IW, IH); if (!s) { printf("mtlprobe: FAIL ioalias IOSurfaceCreate\n"); return 2; }
    io_fill(s, IW, IH, 0);
    id<MTLTexture> a = io_tex(dev, s, IW, IH, MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite | MTLTextureUsageRenderTarget), b = io_tex(dev, s, IW, IH, MTLTextureUsageShaderRead);
    if (!a || !b) { printf("mtlprobe: FAIL ioalias wrappers (%p %p)\n", (__bridge void *)a, (__bridge void *)b); return 2; }
    {
        id<MTLCommandBuffer> cb = [q commandBuffer]; id<MTLBuffer> got = nil;
        io_blit_write(dev, cb, a, IW, IH, S1); io_blit_read_check(dev, cb, b, IW, IH, &got);
        if (!n_run(cb, "ioalias (1)")) { printf("mtlprobe: FAIL ioalias (1) command buffer\n"); return 2; }
        int bad = io_buf_matches(got, IW, IH, S1); printf("mtlprobe: ioalias (1) write via wrapper A, read via wrapper B in one command buffer: %d of %d pixels differ\n", bad, IW * IH); fails += bad != 0;
    }
    // (2) clobber: A wrote pattern S1 (above); now A writes S1b and a NEW wrapper C writes S2 after it, same command buffer
    id<MTLTexture> c = io_tex(dev, s, IW, IH, MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite | MTLTextureUsageRenderTarget);
    {
        id<MTLCommandBuffer> cb = [q commandBuffer];
        io_blit_write(dev, cb, a, IW, IH, S1 + 1); io_blit_write(dev, cb, c, IW, IH, S2);
        if (!n_run(cb, "ioalias (2)")) { printf("mtlprobe: FAIL ioalias (2) command buffer\n"); return 2; }
        NSData *cpu = io_read(s, IW, IH); int bad = 0; const uint8_t *p = cpu.bytes;
        for (int y = 0; y < IH; y++) for (int x = 0; x < IW; x++) { const uint8_t *qq = p + (y * IW + x) * 4; uint8_t e[4] = { (uint8_t)(3 * x + 5 * y + S2), (uint8_t)y, (uint8_t)x, (uint8_t)(255 - ((x ^ y) & 127)) }; if (memcmp(qq, e, 4)) bad++; }
        printf("mtlprobe: ioalias (2) CLOBBER: wrapper A then wrapper C write the same surface: the CPU sees pattern 2 in %d of %d pixels wrong\n", bad, IW * IH); fails += bad != 0;
    }
    // (3) B was touched before; C wrote after: B reads again and must see pattern S2
    {
        id<MTLCommandBuffer> cb = [q commandBuffer]; id<MTLBuffer> got = nil;
        io_blit_read_check(dev, cb, b, IW, IH, &got);
        if (!n_run(cb, "ioalias (3)")) { printf("mtlprobe: FAIL ioalias (3) command buffer\n"); return 2; }
        int bad = io_buf_matches(got, IW, IH, S2); printf("mtlprobe: ioalias (3) a wrapper used earlier reads the surface again after another wrapper wrote: %d of %d pixels differ\n", bad, IW * IH); fails += bad != 0;
    }
    // (3b) the same, inside one command buffer: C writes pattern 3, B (touched before) reads
    {
        id<MTLCommandBuffer> cb = [q commandBuffer]; id<MTLBuffer> got = nil;
        io_blit_write(dev, cb, c, IW, IH, S2 + 5); io_blit_read_check(dev, cb, b, IW, IH, &got);
        if (!n_run(cb, "ioalias (3b)")) { printf("mtlprobe: FAIL ioalias (3b) command buffer\n"); return 2; }
        int bad = io_buf_matches(got, IW, IH, S2 + 5); printf("mtlprobe: ioalias (3b) write via C then read via an earlier-used B in one command buffer: %d of %d pixels differ\n", bad, IW * IH); fails += bad != 0;
    }
    // (4) F1
    const char *fe = getenv("N48M_TEST_IMPORT_FAIL_EVERY");
    if (fe && atoi(fe) > 0) {
        int nilc = 0; for (int i = 0; i < 8; i++) { IOSurfaceRef s2 = io_make(IW, IH); id<MTLTexture> t = io_tex(dev, s2, IW, IH, MTLTextureUsageShaderRead); if (!t) nilc++; }
        printf("mtlprobe: ioalias (4) F1: N48M_TEST_IMPORT_FAIL_EVERY=%s, 8 textures over fresh surfaces: %d nil\n", fe, nilc); fails += nilc != 0;
    } else printf("mtlprobe: ioalias (4) F1 skipped (N48M_TEST_IMPORT_FAIL_EVERY not set)\n");
    printf(fails == 0 ? "mtlprobe: PASS ioalias\n" : "mtlprobe: FAIL ioalias (%d)\n", fails);
    return fails != 0;
}
static int cmd_selcensus(uint64_t rid, int haveRid) {
    id<MTLDevice> dev = pick_device(rid, haveRid); if (!dev) return 2;
    id<MTLCommandQueue> q = [dev newCommandQueue];
    id<MTLBuffer> buf = [dev newBufferWithLength:4096 options:MTLResourceStorageModeShared];
    id<MTLTexture> tex = n_tex(dev, MTLPixelFormatBGRA8Unorm, 64, 64, MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead, MTLStorageModePrivate);
    MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new]; id<MTLSamplerState> smp = [dev newSamplerStateWithDescriptor:sd];
    id<MTLLibrary> lib = n_lib(dev); NSError *err = nil;
    id<MTLFunction> qv = [lib newFunctionWithName:@"blend_vs"], qf = [lib newFunctionWithName:@"blend_fs"];
    MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
    pd.vertexFunction = qv; pd.fragmentFunction = qf; pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    id<MTLRenderPipelineState> pso = [dev newRenderPipelineStateWithDescriptor:pd error:&err];
    id<MTLComputePipelineState> cpso = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"fill_buf"] error:&err];
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = tex; rp.colorAttachments[0].loadAction = MTLLoadActionClear; rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLCommandBuffer> cb = [q commandBuffer];
    id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:rp]; [re endEncoding];
    id<MTLBlitCommandEncoder> be = [cb blitCommandEncoder]; [be endEncoding];
    id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder]; [ce endEncoding];
    id dss = nil, fence = nil, evt = nil, sevt = nil, heap = nil;
    @try { if (getenv("SC_SKIP_DSS")) @throw @"skipped"; MTLDepthStencilDescriptor *dd = [MTLDepthStencilDescriptor new]; dss = [dev newDepthStencilStateWithDescriptor:dd]; } @catch (id e) { printf("SC-NOTE newDepthStencilState threw %s\n", [[e description] UTF8String]); }
    @try { if (!getenv("SC_SKIP_FENCE")) fence = [dev newFence]; } @catch (id e) { printf("SC-NOTE newFence threw %s\n", [[e description] UTF8String]); }
    @try { if (!getenv("SC_SKIP_EVENT")) evt = [dev newEvent]; } @catch (id e) { printf("SC-NOTE newEvent threw %s\n", [[e description] UTF8String]); }
    @try { if (!getenv("SC_SKIP_SEVENT")) sevt = [dev newSharedEvent]; } @catch (id e) { printf("SC-NOTE newSharedEvent threw %s\n", [[e description] UTF8String]); }
    @try { if (getenv("SC_SKIP_HEAP")) @throw @"skipped"; MTLHeapDescriptor *hd = [MTLHeapDescriptor new]; hd.size = 65536; hd.storageMode = MTLStorageModePrivate; heap = [dev newHeapWithDescriptor:hd]; } @catch (id e) { printf("SC-NOTE newHeap threw %s\n", [[e description] UTF8String]); }
    printf("SC-NOTE objects: queue=%s buf=%s tex=%s smp=%s rpso=%s cpso=%s cb=%s re=%s be=%s ce=%s dss=%s fence=%s event=%s sharedevent=%s heap=%s\n",
        q ? class_getName([(id)q class]) : "NIL", buf ? class_getName([(id)buf class]) : "NIL", tex ? class_getName([(id)tex class]) : "NIL", smp ? class_getName([(id)smp class]) : "NIL",
        pso ? class_getName([(id)pso class]) : "NIL", cpso ? class_getName([(id)cpso class]) : "NIL", cb ? class_getName([(id)cb class]) : "NIL",
        re ? class_getName([(id)re class]) : "NIL", be ? class_getName([(id)be class]) : "NIL", ce ? class_getName([(id)ce class]) : "NIL",
        dss ? class_getName([dss class]) : "NIL", fence ? class_getName([fence class]) : "NIL", evt ? class_getName([evt class]) : "NIL", sevt ? class_getName([sevt class]) : "NIL", heap ? class_getName([heap class]) : "NIL");
    struct { const char *n; id o; const char *protos[8]; } T[] = {
        { "device", dev, { "MTLDevice", "MTLDeviceSPI", NULL } },
        { "queue", q, { "MTLCommandQueue", "MTLCommandQueueSPI", NULL } },
        { "commandbuffer", cb, { "MTLCommandBuffer", "MTLCommandBufferSPI", NULL } },
        { "renderenc", re, { "MTLRenderCommandEncoder", "MTLCommandEncoder", "MTLRenderCommandEncoderSPI", "MTLCommandEncoderSPI", NULL } },
        { "blitenc", be, { "MTLBlitCommandEncoder", "MTLCommandEncoder", "MTLBlitCommandEncoderSPI", NULL } },
        { "computeenc", ce, { "MTLComputeCommandEncoder", "MTLCommandEncoder", "MTLComputeCommandEncoderSPI", NULL } },
        { "texture", tex, { "MTLTexture", "MTLResource", "MTLTextureSPI", "MTLResourceSPI", NULL } },
        { "buffer", buf, { "MTLBuffer", "MTLResource", "MTLBufferSPI", "MTLResourceSPI", NULL } },
        { "sampler", smp, { "MTLSamplerState", NULL } },
        { "renderpso", pso, { "MTLRenderPipelineState", NULL } },
        { "computepso", cpso, { "MTLComputePipelineState", NULL } },
        { "depthstencil", dss, { "MTLDepthStencilState", NULL } },
        { "fence", fence, { "MTLFence", NULL } },
        { "event", evt, { "MTLEvent", NULL } },
        { "sharedevent", sevt, { "MTLSharedEvent", "MTLEvent", NULL } },
        { "heap", heap, { "MTLHeap", NULL } },
    };
    for (unsigned i = 0; i < sizeof T / sizeof T[0]; i++) {
        if (!T[i].o) { printf("SC-NOTE %s: object creation returned nil; its protocols are listed against a nil object as all MISSING-CREATE\n", T[i].n); printf("SC\t%s\t-\t-\t-\tNOOBJECT\t\n", T[i].n); continue; }
        for (int k = 0; T[i].protos[k]; k++) sc_proto(T[i].n, T[i].o, T[i].protos[k]);
    }
    // SC_SELS=<file>: lines "kind<TAB>selector" (kind = a name from the table above), checked against the live object whatever the protocol (driver-private selectors).
    if (getenv("SC_SELS")) {
        FILE *f = fopen(getenv("SC_SELS"), "r"); char line[512];
        while (f && fgets(line, sizeof line, f)) {
            char *tab = strchr(line, '\t'); if (!tab) continue; *tab = 0; char *sn = tab + 1; sn[strcspn(sn, "\r\n")] = 0;
            id o = nil; for (unsigned i = 0; i < sizeof T / sizeof T[0]; i++) if (!strcmp(T[i].n, line)) o = T[i].o;
            if (!o) { printf("SX\t%s\t%s\tNOOBJECT\t\n", line, sn); continue; }
            const char *img; const char *own = sc_owner(o, sel_registerName(sn), &img);
            printf("SX\t%s\t%s\t%s\t%s\n", line, sn, own ? own : "MISSING", img);
        }
        if (f) fclose(f);
    }
    return 0;
}

// classdump (#11 prep): every instance method (selector, type encoding) of the named classes, for the signatures of driver-private selectors.
static int cmd_classdump(int n, char **names) {
    for (int i = 0; i < n; i++) {
        Class c = objc_getClass(names[i]);
        if (!c) { printf("CD\t%s\t(no such class)\n", names[i]); continue; }
        unsigned cnt = 0; Method *ml = class_copyMethodList(c, &cnt);
        for (unsigned k = 0; k < cnt; k++) printf("CD\t%s\t%s\t%s\n", names[i], sel_getName(method_getName(ml[k])), method_getTypeEncoding(ml[k]));
        free(ml);
    }
    return 0;
}
// ---- apigaps (#11 prep, Metal API gap census fixes): one oracle per fixed item. Every CPU reference and every Apple-defined value was first checked
// on an Apple-silicon Mac (mtlprobe-arm64, real Metal); on the PC the same code runs through the bundle. Items whose expectation is bundle-specific say so. ----
static int ag_fails;
#define AG_CHECK(tag, cond, ...) do { int ok_ = (cond) ? 1 : 0; printf("mtlprobe: apigaps %s: %s: ", ok_ ? "ok  " : "FAIL", tag); printf(__VA_ARGS__); printf("\n"); if (!ok_) ag_fails++; } while (0)
#define AG_INFO(tag, ...) do { printf("mtlprobe: apigaps info: %s: ", tag); printf(__VA_ARGS__); printf("\n"); } while (0)

static id<MTLRenderPipelineState> ag_pso(id<MTLDevice> dev, id<MTLLibrary> lib, BOOL blend) {
    MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
    pd.vertexFunction = [lib newFunctionWithName:@"blend_vs"]; pd.fragmentFunction = [lib newFunctionWithName:@"blend_fs"];
    MTLRenderPipelineColorAttachmentDescriptor *ca = pd.colorAttachments[0];
    ca.pixelFormat = MTLPixelFormatBGRA8Unorm; ca.blendingEnabled = blend;
    if (blend) { ca.rgbBlendOperation = MTLBlendOperationAdd; ca.alphaBlendOperation = MTLBlendOperationAdd; ca.sourceRGBBlendFactor = MTLBlendFactorOne; ca.sourceAlphaBlendFactor = MTLBlendFactorOne;
        ca.destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha; ca.destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha; }
    NSError *err = nil; id<MTLRenderPipelineState> p = [dev newRenderPipelineStateWithDescriptor:pd error:&err]; perr("ag pipeline", err); return p;
}
typedef struct { int x0, y0, x1, y1; float c[4]; } AgQuad;
static void ag_draw_quad(id<MTLRenderCommandEncoder> re, const AgQuad *q) {
    float rect[4] = { q->x0 / 128.0f - 1.0f, 1.0f - q->y1 / 128.0f, (q->x1 - q->x0) / 128.0f, (q->y1 - q->y0) / 128.0f };
    [re setVertexBytes:rect length:sizeof rect atIndex:0]; [re setFragmentBytes:q->c length:sizeof q->c atIndex:0];
    [re drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
}
// CPU reference of ag quads over a cleared target; blend YES = premultiplied source-over with an 8-bit store between quads (as cmd_blend), NO = replace (rgb*a, a).
static void ag_expect(uint8_t *exp, const uint8_t clr[4] /*BGRA*/, const AgQuad *qs, int nq, BOOL blend) {
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        uint8_t q8[4] = { clr[0], clr[1], clr[2], clr[3] };
        for (int k = 0; k < nq; k++) if (x >= qs[k].x0 && x < qs[k].x1 && y >= qs[k].y0 && y < qs[k].y1) {
            const float *c = qs[k].c; float a = c[3]; float s[4] = { c[2] * a, c[1] * a, c[0] * a, a }, d[4];
            for (int m = 0; m < 4; m++) { d[m] = blend ? s[m] + (q8[m] / 255.0f) * (1.0f - a) : s[m]; float v = d[m] < 0 ? 0 : d[m] > 1 ? 1 : d[m]; q8[m] = (uint8_t)lrintf(v * 255.0f); }
        }
        memcpy(exp + (y * W + x) * 4, q8, 4);
    }
}
static id<MTLRenderCommandEncoder> ag_begin(id<MTLCommandBuffer> cb, id<MTLTexture> tgt, BOOL clear, const uint8_t clr[4]) {
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = tgt; rp.colorAttachments[0].loadAction = clear ? MTLLoadActionClear : MTLLoadActionLoad; rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    rp.colorAttachments[0].clearColor = MTLClearColorMake(clr[2] / 255.0, clr[1] / 255.0, clr[0] / 255.0, clr[3] / 255.0);
    return [cb renderCommandEncoderWithDescriptor:rp];
}
static int ag_compare(const char *tag, id<MTLDevice> dev, id<MTLCommandQueue> q, id<MTLTexture> tgt, const uint8_t *exp, int tol) {
    NSData *got = n_readback(dev, q, tgt, W, H, 4);
    if (!got) { AG_CHECK(tag, 0, "readback failed"); return 1; }
    int d = n_report_diff(tag, got.bytes, exp, W * H, tol, NULL, W, H);
    AG_CHECK(tag, d == 0, "%d of %d pixels differ (tolerance %d LSB)", d, W * H, tol);
    return d != 0;
}

// (1) depth/stencil state objects
static void ag_dss(id<MTLDevice> dev, id<MTLLibrary> lib, id<MTLCommandQueue> q) {
    MTLDepthStencilDescriptor *dd = [MTLDepthStencilDescriptor new];
    // Apple-silicon finding: a NON-default state (depth Less, or stencil Never) makes Apple's GPU drop the draws of a pass that has no depth/stencil attachment, so only
    // states that do not test (Always, no stencil) are DRAWN with; the Less/Never descriptor below is created (and its label read) but never applied.
    dd.depthCompareFunction = MTLCompareFunctionAlways; dd.depthWriteEnabled = NO; dd.label = @"n48-dss";
    MTLDepthStencilDescriptor *dx = [MTLDepthStencilDescriptor new]; dx.depthCompareFunction = MTLCompareFunctionLess; dx.depthWriteEnabled = YES; dx.label = @"n48-dss-less";
    dx.frontFaceStencil.stencilCompareFunction = MTLCompareFunctionAlways; dx.backFaceStencil.stencilCompareFunction = MTLCompareFunctionNever;
    id<MTLDepthStencilState> sx = [dev newDepthStencilStateWithDescriptor:dx];
    AG_CHECK("dss create (Less/Never, not applied)", sx && [sx.label isEqualToString:@"n48-dss-less"], "non-nil %d, label %s", sx != nil, sx.label.UTF8String);
    id<MTLDepthStencilState> s1 = [dev newDepthStencilStateWithDescriptor:dd];
    MTLDepthStencilDescriptor *d2 = [MTLDepthStencilDescriptor new];   // the compositor's kind: everything default
    id<MTLDepthStencilState> s2 = [dev newDepthStencilStateWithDescriptor:d2];
    AG_CHECK("dss create", s1 && s2, "non-nil %d %d, class %s", s1 != nil, s2 != nil, s1 ? class_getName([(id)s1 class]) : "-");
    if (!s1 || !s2) return;
    AG_CHECK("dss device", s1.device == dev, "device identity");
    AG_CHECK("dss label", [s1.label isEqualToString:@"n48-dss"] && s2.label == nil, "label '%s' / default %s", s1.label.UTF8String, s2.label ? "set" : "nil");
    id<MTLRenderPipelineState> pso = ag_pso(dev, lib, NO); if (!pso) { AG_CHECK("dss pipeline", 0, "NIL"); return; }
    id<MTLTexture> tgt = n_tex(dev, MTLPixelFormatBGRA8Unorm, W, H, MTLTextureUsageRenderTarget, MTLStorageModePrivate);
    AgQuad qs[3] = { { 20, 20, 120, 120, { 1.0f, 0.0f, 0.2f, 1.0f } }, { 100, 100, 200, 200, { 0.0f, 1.0f, 0.4f, 1.0f } }, { 60, 150, 240, 230, { 0.4f, 0.2f, 1.0f, 1.0f } } };
    const uint8_t clr[4] = { 60, 40, 20, 255 };
    id<MTLCommandBuffer> cb = [q commandBuffer]; id<MTLRenderCommandEncoder> re = ag_begin(cb, tgt, YES, clr);
    [re setRenderPipelineState:pso];
    [re setDepthStencilState:s1]; [re setStencilReferenceValue:3]; ag_draw_quad(re, &qs[0]);
    [re setDepthStencilState:s2]; ag_draw_quad(re, &qs[1]);
    [re setDepthStencilState:s1]; ag_draw_quad(re, &qs[2]);
    [re endEncoding];
    if (!n_run(cb, "dss")) { AG_CHECK("dss draw", 0, "command buffer failed"); return; }
    static uint8_t exp[W * H * 4]; ag_expect(exp, clr, qs, 3, NO);
    ag_fails += ag_compare("dss pixels", dev, q, tgt, exp, 0);
}

// (2) MTLResource public/SPI surface on textures and buffers
static void ag_res_one(const char *what, id res, BOOL isBuf) {
    char t[96];
    SEL sHeap = @selector(heap), sOff = @selector(heapOffset);
    snprintf(t, sizeof t, "%s heap", what); AG_CHECK(t, [res heap] == nil && [res heapOffset] == 0, "heap nil, heapOffset 0");
    snprintf(t, sizeof t, "%s aliasable", what); BOOL a0 = [res isAliasable]; [res makeAliasable]; BOOL a1 = [res isAliasable];
    AG_CHECK(t, !a0 && !a1, "isAliasable %d, after makeAliasable %d (Apple: only heap resources can be aliasable; both NO here)", a0, a1);
    (void)sHeap; (void)sOff;
    // purgeable state: Metal returns the PREVIOUS state; the default is NonVolatile (2)
    NSUInteger p0 = [res setPurgeableState:MTLPurgeableStateKeepCurrent], p1 = [res setPurgeableState:MTLPurgeableStateVolatile], p2 = [res setPurgeableState:MTLPurgeableStateKeepCurrent], p3 = [res setPurgeableState:MTLPurgeableStateNonVolatile], p4 = [res setPurgeableState:MTLPurgeableStateKeepCurrent];
    snprintf(t, sizeof t, "%s purgeable", what); AG_INFO(t, "KeepCurrent->%lu Volatile->%lu KeepCurrent->%lu NonVolatile->%lu KeepCurrent->%lu", (unsigned long)p0, (unsigned long)p1, (unsigned long)p2, (unsigned long)p3, (unsigned long)p4);
    AG_CHECK(t, p0 == 2 && p1 == 2 && p2 == 2 && p3 == 2 && p4 == 2, "a non-heap resource stays NonVolatile (2) whatever is requested (Apple Apple-silicon)");
    struct { const char *s; } spi[] = { { "protectionOptions" }, { "responsibleProcess" }, { "isComplete" }, { "isWriteComplete" }, { "unfilteredResourceOptions" }, { "isPurgeable" } };
    for (unsigned i = 0; i < sizeof spi / sizeof spi[0]; i++) {
        SEL sel = sel_registerName(spi[i].s); snprintf(t, sizeof t, "%s SPI %s", what, spi[i].s);
        if (![res respondsToSelector:sel]) { AG_INFO(t, "not implemented by %s (SPI; skipped)", class_getName(object_getClass(res))); continue; }
        unsigned long long v = ((unsigned long long (*)(id, SEL))objc_msgSend)(res, sel); v &= (!strcmp(spi[i].s, "responsibleProcess")) ? 0xFFFFFFFFull : (!strncmp(spi[i].s, "is", 2)) ? 0xFFull : ~0ull;
        AG_INFO(t, "= %llu", v);
    }
    SEL sProt = sel_registerName("protectionOptions"), sResp = sel_registerName("responsibleProcess"), sSetResp = sel_registerName("setResponsibleProcess:"), sWait = sel_registerName("waitUntilComplete");
    if ([res respondsToSelector:sProt]) { snprintf(t, sizeof t, "%s protectionOptions", what); AG_CHECK(t, ((NSUInteger (*)(id, SEL))objc_msgSend)(res, sProt) == 0, "0 (no protected content)"); }
    if ([res respondsToSelector:sSetResp]) {
        ((void (*)(id, SEL, int))objc_msgSend)(res, sSetResp, (int)getpid());
        snprintf(t, sizeof t, "%s setResponsibleProcess", what); int rp = [res respondsToSelector:sResp] ? ((int (*)(id, SEL))objc_msgSend)(res, sResp) : -1;
        AG_CHECK(t, rp == (int)getpid(), "set %d, read back %d", (int)getpid(), rp);
    }
    if ([res respondsToSelector:sResp]) { snprintf(t, sizeof t, "%s responsibleProcess default", what); AG_CHECK(t, ((int (*)(id, SEL))objc_msgSend)(res, sResp) == (int)getpid(), "the creating process (Apple Apple-silicon)"); }
    if ([res respondsToSelector:sWait]) ((void (*)(id, SEL))objc_msgSend)(res, sWait);
    if (isBuf) { snprintf(t, sizeof t, "%s allocatedSize", what); AG_CHECK(t, [(id<MTLBuffer>)res allocatedSize] >= 4096, "%lu >= 4096", (unsigned long)[(id<MTLBuffer>)res allocatedSize]); }
    snprintf(t, sizeof t, "%s setOwnerWithIdentity", what);
    if ([res respondsToSelector:@selector(setOwnerWithIdentity:)]) { int rc = [res setOwnerWithIdentity:0]; AG_CHECK(t, rc == (int)0xE00002C2, "returns kIOReturnBadArgument 0x%x for a non-heap resource (Apple Apple-silicon)", (unsigned)rc); }
}
static void ag_res(id<MTLDevice> dev) {
    id<MTLBuffer> b = [dev newBufferWithLength:4096 options:MTLResourceStorageModeShared];
    id<MTLTexture> t = n_tex(dev, MTLPixelFormatRGBA8Unorm, 16, 16, MTLTextureUsageShaderRead, MTLStorageModePrivate);
    AG_CHECK("res objects", b && t, "buffer %d texture %d", b != nil, t != nil);
    if (!b || !t) return;
    ag_res_one("buffer", b, YES); ag_res_one("texture", t, NO);
    id<MTLBuffer> c = [dev newBufferWithLength:4096 options:MTLResourceStorageModeShared];
    SEL sVA = sel_registerName("virtualAddress");
    if ([c respondsToSelector:sVA]) { void *va = ((void *(*)(id, SEL))objc_msgSend)(c, sVA); AG_INFO("buffer virtualAddress", "%p (contents %p)", va, c.contents); }
    else AG_INFO("buffer virtualAddress", "not implemented by %s", class_getName(object_getClass(c)));
    [b addDebugMarker:@"m" range:NSMakeRange(0, 16)]; [b removeAllDebugMarkers];
    AG_CHECK("buffer debug markers", YES, "addDebugMarker / removeAllDebugMarkers did not raise");
}

// (3) queue SPI + command buffer protection options
static void ag_queue(id<MTLDevice> dev, id<MTLCommandQueue> q) {
    SEL sGP = sel_registerName("setGPUPriority:"), sBGP = sel_registerName("setBackgroundGPUPriority:"), sGet = sel_registerName("getGPUPriority"), sCQ = sel_registerName("setCompletionQueue:"), sBoth = sel_registerName("_setGPUPriority:backgroundPriority:");
    if ([(id)q respondsToSelector:sGP]) { BOOL r = ((BOOL (*)(id, SEL, NSUInteger))objc_msgSend)(q, sGP, 2); AG_CHECK("queue setGPUPriority:2", r == YES, "returned %d (Apple Apple-silicon: YES); class %s", r, class_getName(object_getClass(q))); } else AG_INFO("queue setGPUPriority:", "not implemented by %s", class_getName(object_getClass(q)));
    if ([(id)q respondsToSelector:sBGP]) { BOOL r = ((BOOL (*)(id, SEL, NSUInteger))objc_msgSend)(q, sBGP, 1); AG_CHECK("queue setBackgroundGPUPriority:1", r == NO, "returned %d (Apple Apple-silicon: NO)", r); }
    if ([(id)q respondsToSelector:sBoth]) { BOOL r = ((BOOL (*)(id, SEL, NSUInteger, NSUInteger))objc_msgSend)(q, sBoth, 2, 1); AG_CHECK("queue _setGPUPriority:backgroundPriority:", r == NO, "returned %d (Apple Apple-silicon: NO)", r); }
    if ([(id)q respondsToSelector:sGet]) { NSUInteger g = ((NSUInteger (*)(id, SEL))objc_msgSend)(q, sGet); AG_CHECK("queue getGPUPriority", g == 2, "= %lu (the value set by setGPUPriority:2 on an Apple-silicon Mac)", (unsigned long)g); }
    if ([(id)q respondsToSelector:sCQ]) { ((void (*)(id, SEL, id))objc_msgSend)(q, sCQ, dispatch_queue_create("ag.cq", DISPATCH_QUEUE_SERIAL)); AG_INFO("queue setCompletionQueue:", "accepted"); }
    // the queue must still work after the setters
    id<MTLCommandBuffer> cb = [q commandBuffer]; BOOL ok = n_run(cb, "queue after SPI setters");
    AG_CHECK("queue SPI setters", ok, "empty command buffer still completes after the SPI calls (no selector raised)");
    SEL sPO = sel_registerName("protectionOptions"), sSPO = sel_registerName("setProtectionOptions:");
    cb = [q commandBuffer];
    if ([(id)cb respondsToSelector:sPO]) { NSUInteger v0 = ((NSUInteger (*)(id, SEL))objc_msgSend)(cb, sPO); ((void (*)(id, SEL, NSUInteger))objc_msgSend)(cb, sSPO, 0); NSUInteger v1 = ((NSUInteger (*)(id, SEL))objc_msgSend)(cb, sPO);
        AG_CHECK("cb protectionOptions", v0 == 0 && v1 == 0, "default %lu, after set(0) %lu", (unsigned long)v0, (unsigned long)v1); }
    else AG_INFO("cb protectionOptions", "not implemented by %s", class_getName(object_getClass(cb)));
    [cb commit]; [cb waitUntilCompleted]; AG_CHECK("cb after protectionOptions", cb.status == MTLCommandBufferStatusCompleted, "status %ld", (long)cb.status);
}

// (4) render encoder barriers, residency, store actions, fences between passes
static void ag_barrier(id<MTLDevice> dev, id<MTLLibrary> lib, id<MTLCommandQueue> q) {
    id<MTLRenderPipelineState> pso = ag_pso(dev, lib, YES); if (!pso) { AG_CHECK("barrier pipeline", 0, "NIL"); return; }
    id<MTLTexture> tgt = n_tex(dev, MTLPixelFormatBGRA8Unorm, W, H, MTLTextureUsageRenderTarget, MTLStorageModePrivate);
    AgQuad qs[4] = { { 40, 40, 168, 168, { 0.8f, 0.2f, 0.1f, 0.5f } }, { 88, 88, 216, 216, { 0.1f, 0.6f, 0.9f, 0.25f } }, { 20, 120, 150, 240, { 0.9f, 0.9f, 0.2f, 0.75f } }, { 130, 20, 240, 130, { 0.3f, 0.3f, 0.3f, 0.6f } } };
    const uint8_t clr[4] = { 60, 40, 20, 255 };
    id<MTLBuffer> helper = [dev newBufferWithLength:256 options:MTLResourceStorageModeShared];
    id<MTLFence> fence = [dev newFence]; AG_CHECK("fence create", fence != nil, "class %s", fence ? class_getName([(id)fence class]) : "-");
    if (fence) { fence.label = @"n48-fence"; AG_CHECK("fence label/device", [fence.label isEqualToString:@"n48-fence"] && fence.device == dev, "label + device identity"); }
    id<MTLCommandBuffer> cb = [q commandBuffer]; id<MTLRenderCommandEncoder> re = ag_begin(cb, tgt, YES, clr);
    [re setRenderPipelineState:pso];
    [re useResource:helper usage:MTLResourceUsageRead]; [re setColorStoreAction:MTLStoreActionStore atIndex:0];
    ag_draw_quad(re, &qs[0]);
    [re memoryBarrierWithScope:MTLBarrierScopeRenderTargets afterStages:MTLRenderStageFragment beforeStages:MTLRenderStageVertex];
    ag_draw_quad(re, &qs[1]);
    if ([(id)re respondsToSelector:@selector(textureBarrier)]) { [re textureBarrier]; AG_INFO("textureBarrier", "called"); }
    [re memoryBarrierWithResources:(id<MTLResource> __unsafe_unretained[]){ tgt } count:1 afterStages:MTLRenderStageFragment beforeStages:MTLRenderStageVertex];
    ag_draw_quad(re, &qs[2]);
    if (fence) [re updateFence:fence afterStages:MTLRenderStageFragment];
    [re endEncoding];
    { id<MTLRenderCommandEncoder> r2 = ag_begin(cb, tgt, NO, clr); if (fence) [r2 waitForFence:fence beforeStages:MTLRenderStageVertex];
      [r2 setRenderPipelineState:pso]; ag_draw_quad(r2, &qs[3]); [r2 endEncoding]; }
    if (!n_run(cb, "barrier")) { AG_CHECK("barrier draw", 0, "command buffer failed"); return; }
    static uint8_t exp[W * H * 4]; ag_expect(exp, clr, qs, 4, YES);
    ag_fails += ag_compare("barrier pixels (blend, tolerance 1 LSB)", dev, q, tgt, exp, 1);
}

// (5) fences across blit / compute / render encoders
static void ag_fence(id<MTLDevice> dev, id<MTLLibrary> lib, id<MTLCommandQueue> q) {
    id<MTLFence> f = [dev newFence]; if (!f) { AG_CHECK("fence2 create", 0, "nil"); return; }
    id<MTLBuffer> a = [dev newBufferWithLength:4096 options:MTLResourceStorageModeShared], b = [dev newBufferWithLength:4096 options:MTLResourceStorageModeShared], c = [dev newBufferWithLength:4096 options:MTLResourceStorageModeShared];
    memset(b.contents, 0, 4096); memset(c.contents, 0, 4096);
    NSError *err = nil; id<MTLComputePipelineState> cp = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"fill_buf"] error:&err];
    id<MTLCommandBuffer> cb = [q commandBuffer];
    id<MTLBlitCommandEncoder> be = [cb blitCommandEncoder]; [be fillBuffer:a range:NSMakeRange(0, 4096) value:0x5A]; [be updateFence:f]; [be endEncoding];
    id<MTLBlitCommandEncoder> b2 = [cb blitCommandEncoder]; [b2 waitForFence:f]; [b2 copyFromBuffer:a sourceOffset:0 toBuffer:b destinationOffset:0 size:4096]; [b2 updateFence:f]; [b2 endEncoding];
    if (cp) { id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder]; [ce waitForFence:f]; [ce setComputePipelineState:cp]; [ce setBuffer:c offset:0 atIndex:0];
        uint32_t n = 1024; [ce setBytes:&n length:4 atIndex:1]; [ce dispatchThreadgroups:MTLSizeMake(16, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)]; [ce updateFence:f]; [ce endEncoding]; }
    id<MTLBlitCommandEncoder> b3 = [cb blitCommandEncoder]; [b3 waitForFence:f]; [b3 copyFromBuffer:b sourceOffset:0 toBuffer:a destinationOffset:0 size:4096]; [b3 endEncoding];
    BOOL ok = n_run(cb, "fences");
    uint8_t want[4096]; memset(want, 0x5A, sizeof want); int bad = 0; for (int i = 0; i < 1024; i++) { uint32_t g = (uint32_t)i; uint32_t e = g * 2654435761u ^ (g >> 3); if (((uint32_t *)c.contents)[i] != e) bad++; }
    AG_CHECK("fence chain", ok && !memcmp(b.contents, want, 4096) && !memcmp(a.contents, want, 4096) && (!cp || !bad), "blit fill -> update/wait -> blit copy -> compute fill_buf -> blit copy: copies %s, compute %d bad words", !memcmp(b.contents, want, 4096) ? "exact" : "WRONG", bad);
}

// (6) events: encodeSignalEvent / encodeWaitForEvent across command buffers, queues and the host
static void ag_event(id<MTLDevice> dev) {
    id<MTLCommandQueue> q1 = [dev newCommandQueue], q2 = [dev newCommandQueue];
    id<MTLSharedEvent> ev = [dev newSharedEvent]; AG_CHECK("event create", ev != nil, "newSharedEvent class %s", ev ? class_getName([(id)ev class]) : "-");
    id<MTLEvent> pe = [dev newEvent]; AG_CHECK("plain event create", pe != nil, "newEvent class %s", pe ? class_getName([(id)pe class]) : "-");
    if (!ev || !pe) return;
    // E1: cb B (queue 2) waits for value 1 and is committed FIRST; cb A (queue 1) signals it 0.3 s later
    id<MTLCommandBuffer> B = [q2 commandBuffer]; [B encodeWaitForEvent:ev value:1];
    id<MTLCommandBuffer> A = [q1 commandBuffer]; [A encodeSignalEvent:ev value:1];
    double t0 = now_s(); [B commit]; usleep(300000); double tA = now_s() - t0;
    MTLCommandBufferStatus sBefore = B.status;
    [A commit]; [A waitUntilCompleted]; [B waitUntilCompleted]; double tB = now_s() - t0;
    AG_CHECK("event E1 cross-queue", A.status == MTLCommandBufferStatusCompleted && B.status == MTLCommandBufferStatusCompleted && sBefore != MTLCommandBufferStatusCompleted && tB >= tA - 0.01 && ev.signaledValue == 1,
             "B status before the signal %ld (not Completed), A/B Completed, B finished %.3f s after commit (A committed at %.3f s), signaledValue %llu", (long)sBefore, tB, tA, (unsigned long long)ev.signaledValue);
    // E2: a cb waiting for value 5 is released by the HOST
    id<MTLCommandBuffer> C = [q1 commandBuffer]; [C encodeWaitForEvent:ev value:5]; [C commit]; usleep(300000);
    MTLCommandBufferStatus sC = C.status;
    ev.signaledValue = 5;
    double w0 = now_s(); while (C.status != MTLCommandBufferStatusCompleted && C.status != MTLCommandBufferStatusError && now_s() - w0 < 5.0) usleep(2000);
    AG_CHECK("event E2 host release", sC != MTLCommandBufferStatusCompleted && C.status == MTLCommandBufferStatusCompleted, "status while waiting %ld, after setSignaledValue:5 %ld in %.3f s", (long)sC, (long)C.status, now_s() - w0);
    // E3: plain MTLEvent, one queue, signal in one cb, wait in the next (a render pass after the wait)
    id<MTLCommandBuffer> S = [q1 commandBuffer]; [S encodeSignalEvent:pe value:7];
    id<MTLTexture> tgt = n_tex(dev, MTLPixelFormatBGRA8Unorm, 16, 16, MTLTextureUsageRenderTarget, MTLStorageModePrivate);
    id<MTLCommandBuffer> Wt = [q1 commandBuffer]; [Wt encodeWaitForEvent:pe value:7];
    { MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor]; rp.colorAttachments[0].texture = tgt; rp.colorAttachments[0].loadAction = MTLLoadActionClear;
      rp.colorAttachments[0].storeAction = MTLStoreActionStore; id<MTLRenderCommandEncoder> re = [Wt renderCommandEncoderWithDescriptor:rp]; [re endEncoding]; }
    [S commit]; [Wt commit]; [Wt waitUntilCompleted];
    AG_CHECK("event E3 same queue plain event", S.status == MTLCommandBufferStatusCompleted && Wt.status == MTLCommandBufferStatusCompleted, "signal cb %ld, wait cb %ld", (long)S.status, (long)Wt.status);
    // E4: signal then host-side wait API
    id<MTLCommandBuffer> D = [q1 commandBuffer]; [D encodeSignalEvent:ev value:9]; [D commit];
    BOOL got = [ev waitUntilSignaledValue:9 timeoutMS:5000];
    AG_CHECK("event E4 host waits for a cb signal", got && ev.signaledValue == 9, "waitUntilSignaledValue:9 -> %d, value %llu", got, (unsigned long long)ev.signaledValue);
}

// (7) device limits and properties
static void ag_limits(id<MTLDevice> dev) {
    MTLSize m = dev.maxThreadsPerThreadgroup; NSUInteger tg = dev.maxThreadgroupMemoryLength;
    AG_INFO("device limits", "maxThreadgroupMemoryLength %lu, maxThreadsPerThreadgroup %lux%lux%lu, supportsSampleCount 1/2/4/8 = %d/%d/%d/%d, minimumTextureBufferAlignment(BGRA8) %lu, depth24stencil8 %d",
            (unsigned long)tg, (unsigned long)m.width, (unsigned long)m.height, (unsigned long)m.depth, [dev supportsTextureSampleCount:1], [dev supportsTextureSampleCount:2], [dev supportsTextureSampleCount:4], [dev supportsTextureSampleCount:8],
            (unsigned long)[dev minimumTextureBufferAlignmentForPixelFormat:MTLPixelFormatBGRA8Unorm], [dev isDepth24Stencil8PixelFormatSupported]);
    { NSUInteger c0 = dev.currentAllocatedSize, c1 = 0, c2 = 0; id<MTLBuffer> big = nil;
      @autoreleasepool { big = [dev newBufferWithLength:(8u << 20) options:MTLResourceStorageModePrivate]; (void)big; c1 = dev.currentAllocatedSize; big = nil; }
      c2 = dev.currentAllocatedSize;
      AG_CHECK("device currentAllocatedSize", c1 >= c0 + (8u << 20) && c2 < c1, "before %lu, with an 8 MiB Private buffer %lu (+%lu), after release %lu", (unsigned long)c0, (unsigned long)c1, (unsigned long)(c1 - c0), (unsigned long)c2); }
#if defined(__x86_64__)
    AG_CHECK("device limits (bundle)", tg == 32768 && m.width == 1024 && m.height == 1024 && m.depth == 1024 && [dev supportsTextureSampleCount:1] && ![dev supportsTextureSampleCount:4] && [dev minimumTextureBufferAlignmentForPixelFormat:MTLPixelFormatBGRA8Unorm] == 256 && ![dev isDepth24Stencil8PixelFormatSupported],
             "32768, 1024^3, sample counts {1}, alignment 256, no D24S8");
    // the private selector Metal itself sends (census: GFX10_MtlDevice -maxThreadgroupMemoryLength 15987 calls) and supportsSampleCount: (Metal, 8 calls at start-up)
    SEL sS = sel_registerName("supportsSampleCount:");
    AG_CHECK("device supportsSampleCount: (private)", [(id)dev respondsToSelector:sS] && ((BOOL (*)(id, SEL, NSUInteger))objc_msgSend)(dev, sS, 1) && !((BOOL (*)(id, SEL, NSUInteger))objc_msgSend)(dev, sS, 4), "1 YES, 4 NO");
#else
    AG_CHECK("device limits (Apple reference)", tg == 32768 && m.width == 1024 && m.height == 1024 && m.depth == 1024, "an Apple-silicon Mac reports 32768 and 1024x1024x1024, the values the bundle returns");
#endif
}

// (8) texture views
static id<MTLTexture> ag_src_tex(id<MTLDevice> dev, MTLTextureUsage u) {
    id<MTLTexture> t = n_tex(dev, MTLPixelFormatRGBA8Unorm, 64, 64, u, MTLStorageModeShared);
    uint8_t p[64 * 64 * 4]; n_pattern(p); [t replaceRegion:MTLRegionMake2D(0, 0, 64, 64) mipmapLevel:0 withBytes:p bytesPerRow:64 * 4]; return t;
}
// draws `src` (whatever view it is) over the full 64x64 BGRA8 target through quad_vs/quad_fs (nearest sampler) and returns the target bytes
static NSData *ag_sample(id<MTLDevice> dev, id<MTLLibrary> lib, id<MTLCommandQueue> q, id<MTLTexture> src, const char *tag) {
    static id<MTLRenderPipelineState> pso; static id<MTLDevice> pdev;
    if (!pso || pdev != dev) {
        MTLVertexDescriptor *vd = [MTLVertexDescriptor vertexDescriptor];
        vd.attributes[0].format = MTLVertexFormatFloat2; vd.attributes[0].offset = 0; vd.attributes[0].bufferIndex = 0;
        vd.attributes[1].format = MTLVertexFormatFloat2; vd.attributes[1].offset = 8; vd.attributes[1].bufferIndex = 0; vd.layouts[0].stride = 16;
        MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new]; pd.vertexFunction = [lib newFunctionWithName:@"quad_vs"]; pd.fragmentFunction = [lib newFunctionWithName:@"quad_fs"];
        pd.vertexDescriptor = vd; pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm; NSError *err = nil; pso = [dev newRenderPipelineStateWithDescriptor:pd error:&err]; perr("ag quad pipeline", err); pdev = dev;
    }
    if (!pso) return nil;
    MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new]; sd.minFilter = MTLSamplerMinMagFilterNearest; sd.magFilter = MTLSamplerMinMagFilterNearest; id<MTLSamplerState> smp = [dev newSamplerStateWithDescriptor:sd];
    id<MTLTexture> tgt = n_tex(dev, MTLPixelFormatBGRA8Unorm, 64, 64, MTLTextureUsageRenderTarget, MTLStorageModePrivate);
    float v[16] = { -1, -1, 0, 1,   1, -1, 1, 1,   -1, 1, 0, 0,   1, 1, 1, 0 };   // strip: (pos.x, pos.y, u, v) with v flipped so texel (x,y) lands on pixel (x,y)
    float xf[4] = { 1, 1, 0, 0 }, tint[4] = { 1, 1, 1, 1 };
    id<MTLBuffer> vb = [dev newBufferWithBytes:v length:sizeof v options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> cb = [q commandBuffer];
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor]; rp.colorAttachments[0].texture = tgt; rp.colorAttachments[0].loadAction = MTLLoadActionClear; rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:rp];
    [re setRenderPipelineState:pso]; [re setVertexBuffer:vb offset:0 atIndex:0]; [re setVertexBytes:xf length:16 atIndex:1]; [re setFragmentBytes:tint length:16 atIndex:0];
    [re setFragmentTexture:src atIndex:0]; [re setFragmentSamplerState:smp atIndex:0]; [re drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4]; [re endEncoding];
    if (!n_run(cb, tag)) return nil;
    return n_readback(dev, q, tgt, 64, 64, 4);
}
static void ag_views(id<MTLDevice> dev, id<MTLLibrary> lib, id<MTLCommandQueue> q) {
    id<MTLTexture> root = ag_src_tex(dev, MTLTextureUsageShaderRead | MTLTextureUsagePixelFormatView);
    if (!root) { AG_CHECK("view root", 0, "nil"); return; }
    uint8_t src[64 * 64 * 4]; n_pattern(src);   // RGBA memory order
    // plain sample: RGBA texture -> BGRA target swaps R and B
    NSData *plain = ag_sample(dev, lib, q, root, "view plain");
    uint8_t swapped[64 * 64 * 4]; for (int i = 0; i < 64 * 64; i++) { swapped[i*4] = src[i*4+2]; swapped[i*4+1] = src[i*4+1]; swapped[i*4+2] = src[i*4]; swapped[i*4+3] = src[i*4+3]; }
    AG_CHECK("view control (no view)", plain && !memcmp(plain.bytes, swapped, sizeof swapped), "RGBA8 sampled into a BGRA8 target swaps R/B (control)");
    id<MTLTexture> vb = [root newTextureViewWithPixelFormat:MTLPixelFormatBGRA8Unorm];
    AG_CHECK("view create BGRA8", vb != nil, "class %s", vb ? class_getName([(id)vb class]) : "-");
    if (vb) {
        AG_CHECK("view properties", vb.pixelFormat == MTLPixelFormatBGRA8Unorm && vb.width == 64 && vb.height == 64 && vb.textureType == MTLTextureType2D && vb.parentTexture == root && vb.mipmapLevelCount == 1, "pixelFormat %lu, %lux%lu, parentTexture == root %d",
                 (unsigned long)vb.pixelFormat, (unsigned long)vb.width, (unsigned long)vb.height, vb.parentTexture == root);
        NSData *g = ag_sample(dev, lib, q, vb, "view BGRA8");
        AG_CHECK("view BGRA8 pixels", g && !memcmp(g.bytes, src, sizeof src), "a BGRA8 view of RGBA8 memory sampled into a BGRA8 target reproduces the memory bytes exactly");
    }
    MTLTextureSwizzleChannels sw = { MTLTextureSwizzleBlue, MTLTextureSwizzleGreen, MTLTextureSwizzleRed, MTLTextureSwizzleAlpha };
    id<MTLTexture> vs = [root newTextureViewWithPixelFormat:MTLPixelFormatRGBA8Unorm textureType:MTLTextureType2D levels:NSMakeRange(0, 1) slices:NSMakeRange(0, 1) swizzle:sw];
    AG_CHECK("view create swizzle", vs != nil, "RGBA8 view with BGRA swizzle");
    if (vs) { NSData *g = ag_sample(dev, lib, q, vs, "view swizzle");
        AG_CHECK("view swizzle pixels", g && !memcmp(g.bytes, src, sizeof src), "swizzle B,G,R,A reproduces the memory bytes exactly"); }
    id<MTLTexture> vr = [root newTextureViewWithPixelFormat:MTLPixelFormatRGBA8Unorm_sRGB];
    AG_CHECK("view create sRGB", vr != nil, "class %s", vr ? class_getName([(id)vr class]) : "-");
    if (vr) {
        NSData *g = ag_sample(dev, lib, q, vr, "view sRGB");
        static uint8_t exp[64 * 64 * 4];
        for (int i = 0; i < 64 * 64; i++) for (int c = 0; c < 3; c++) { double x = src[i*4+c] / 255.0; double l = x <= 0.04045 ? x / 12.92 : pow((x + 0.055) / 1.055, 2.4); int o = (int)lrint(l * 255.0); exp[i*4 + (c == 0 ? 2 : c == 2 ? 0 : 1)] = (uint8_t)o; }
        for (int i = 0; i < 64 * 64; i++) exp[i*4+3] = src[i*4+3];
        int d1 = 0, d0 = 0, maxd = 0; if (g) for (int i = 0; i < 64 * 64 * 4; i++) { int d = abs((int)((const uint8_t *)g.bytes)[i] - (int)exp[i]); if (d) d0++; if (d > 1) d1++; if (d > maxd) maxd = d; }
        AG_CHECK("view sRGB pixels", g && d1 == 0, "sRGB-decoded samples vs the CPU decode: %d bytes differ, %d beyond 1 LSB, max %d (tolerance 1 LSB)", d0, d1, maxd);
    }
    // a view keeps the root alive and shares its contents: write through the root after the view exists, sample the view again
    if (vb) { uint8_t p2[64 * 64 * 4]; for (int i = 0; i < 64 * 64 * 4; i++) p2[i] = (uint8_t)(255 - src[i]); [root replaceRegion:MTLRegionMake2D(0, 0, 64, 64) mipmapLevel:0 withBytes:p2 bytesPerRow:256];
        NSData *g = ag_sample(dev, lib, q, vb, "view after root write"); AG_CHECK("view shares the root's image", g && !memcmp(g.bytes, p2, sizeof p2), "a replaceRegion on the root is seen through the existing view"); }
    // refusals (the bundle returns nil; bundle-specific, not run against Apple's validation layer on arm64)
#if defined(__x86_64__)
    id<MTLTexture> bad1 = [root newTextureViewWithPixelFormat:MTLPixelFormatR8Unorm];
    id<MTLTexture> plainRoot = ag_src_tex(dev, MTLTextureUsageShaderRead);
    id<MTLTexture> bad2 = [plainRoot newTextureViewWithPixelFormat:MTLPixelFormatBGRA8Unorm];
    id<MTLTexture> same = [plainRoot newTextureViewWithPixelFormat:MTLPixelFormatRGBA8Unorm];
    id<MTLTexture> bad3 = [root newTextureViewWithPixelFormat:MTLPixelFormatRGBA8Unorm textureType:MTLTextureType2D levels:NSMakeRange(0, 2) slices:NSMakeRange(0, 1)];
    AG_CHECK("view refusals (bundle)", !bad1 && !bad2 && same && !bad3, "different bytes per pixel: nil; format change without PixelFormatView usage: nil; same-format view without the usage: ok; two mip levels: nil");
#endif
}

static int cmd_apigaps(uint64_t rid, int haveRid, const char *only) {
    id<MTLDevice> dev = pick_device(rid, haveRid); if (!dev) return 2;
    id<MTLLibrary> lib = n_lib(dev); if (!lib) return 2;
    id<MTLCommandQueue> q = [dev newCommandQueue]; ag_fails = 0;
    #define RUN(nm, call) if (!only || !strcmp(only, nm)) { printf("mtlprobe: apigaps ---- %s\n", nm); call; }
    RUN("dss", ag_dss(dev, lib, q)); RUN("res", ag_res(dev)); RUN("queue", ag_queue(dev, q)); RUN("barrier", ag_barrier(dev, lib, q));
    RUN("fence", ag_fence(dev, lib, q)); RUN("event", ag_event(dev)); RUN("limits", ag_limits(dev)); RUN("views", ag_views(dev, lib, q));
    #undef RUN
    printf("mtlprobe: apigaps: %d check(s) FAILED\n", ag_fails);
    printf(ag_fails == 0 ? "mtlprobe: PASS apigaps\n" : "mtlprobe: FAIL apigaps\n");
    return ag_fails ? 1 : 0;
}

// devcall <selector> [uint-arg] [--registry-id N]: sends ONE integer-returning selector to the device (inherited MTLIOAccelDevice properties may SEGV, so one call per process).
static int cmd_devcall(const char *sel, int hasArg, unsigned long arg, uint64_t rid, int haveRid) {
    id<MTLDevice> dev = pick_device(rid, haveRid); if (!dev) return 2;
    SEL s = sel_registerName(sel);
    if (![(id)dev respondsToSelector:s]) { printf("devcall %s: device does NOT respond (%s)\n", sel, class_getName(object_getClass(dev))); return 1; }
    printf("devcall %s: calling (implementation in %s)\n", sel, class_getName(object_getClass(dev))); fflush(stdout);
    unsigned long long v = hasArg ? ((unsigned long long (*)(id, SEL, unsigned long))objc_msgSend)(dev, s, arg) : ((unsigned long long (*)(id, SEL))objc_msgSend)(dev, s);
    printf("devcall %s = %llu (0x%llx)\n", sel, v, v); return 0;
}

// nocopy [buf|refuse|tex|render|spi] (#11 prep, item 12): client-memory buffers and textures. Public API on both machines (verified on an Apple-silicon Mac first);
// the SPI forms QuartzCore uses (newTiledTextureWithBytesNoCopy...) are exercised on the PC only.
static int nc_freed;
static void *nc_alloc(size_t len) { void *m = NULL; if (posix_memalign(&m, 16384, len)) return NULL; return m; }
static void (^nc_dealloc(void))(void *, NSUInteger) { return ^(void *p, NSUInteger n) { (void)n; nc_freed++; free(p); }; }
static int cmd_nocopy(const char *only, uint64_t rid, int haveRid) {
    id<MTLDevice> dev = pick_device(rid, haveRid); if (!dev) return 2;
    id<MTLLibrary> lib = n_lib(dev); if (!lib) return 2;
    id<MTLCommandQueue> q = [dev newCommandQueue]; ag_fails = 0;
    #define NRUN(nm) (!only || !strcmp(only, nm))
    if (NRUN("buf")) {
        printf("mtlprobe: nocopy ---- buf\n");
        size_t len = 32768; uint8_t *mem = nc_alloc(len); for (size_t i = 0; i < len; i++) mem[i] = (uint8_t)(i * 7 + 3);
        int before = nc_freed;
        @autoreleasepool {
            id<MTLBuffer> b = [dev newBufferWithBytesNoCopy:mem length:len options:MTLResourceStorageModeShared deallocator:nc_dealloc()];
            AG_CHECK("nocopy buffer create", b != nil, "class %s", b ? class_getName([(id)b class]) : "-");
            if (b) {
                AG_CHECK("nocopy buffer contents", b.contents == mem && b.length == len, "contents == the client pointer, length %lu", (unsigned long)b.length);
                id<MTLBuffer> c = [dev newBufferWithLength:len options:MTLResourceStorageModeShared]; memset(c.contents, 0, len);
                id<MTLCommandBuffer> cb = [q commandBuffer]; id<MTLBlitCommandEncoder> be = [cb blitCommandEncoder];
                [be copyFromBuffer:b sourceOffset:0 toBuffer:c destinationOffset:0 size:len]; [be endEncoding];
                BOOL ok = n_run(cb, "nocopy read"); AG_CHECK("nocopy GPU reads client memory", ok && !memcmp(c.contents, mem, len), "32 KiB copied out exact");
                for (size_t i = 0; i < len; i++) mem[i] = (uint8_t)(255 - i);   // CPU rewrite after first use
                cb = [q commandBuffer]; be = [cb blitCommandEncoder]; [be copyFromBuffer:b sourceOffset:0 toBuffer:c destinationOffset:0 size:len]; [be endEncoding];
                ok = n_run(cb, "nocopy reread"); AG_CHECK("nocopy CPU rewrite seen by the GPU", ok && !memcmp(c.contents, mem, len), "second copy-out sees the rewritten memory");
                uint8_t keep[len]; memcpy(keep, mem, len);
                cb = [q commandBuffer]; be = [cb blitCommandEncoder]; [be fillBuffer:b range:NSMakeRange(4096, 4096) value:0x5A]; [be endEncoding];
                ok = n_run(cb, "nocopy write"); memset(keep + 4096, 0x5A, 4096);
                AG_CHECK("nocopy GPU write seen by the CPU", ok && !memcmp(mem, keep, len), "fillBuffer on [4096,8192) visible in the client memory, the rest unchanged");
            }
            b = nil;
        }
        AG_CHECK("nocopy deallocator", nc_freed == before + 1, "called %d time(s) after release (expected 1)", nc_freed - before);
    }
    if (NRUN("refuse")) {
        printf("mtlprobe: nocopy ---- refuse\n");
        uint8_t *mem = nc_alloc(65536);
        id<MTLBuffer> u = [dev newBufferWithBytesNoCopy:mem + 64 length:16384 options:MTLResourceStorageModeShared deallocator:nil];
        id<MTLBuffer> l = [dev newBufferWithBytesNoCopy:mem length:16000 options:MTLResourceStorageModeShared deallocator:nil];
#if defined(__x86_64__)
        AG_CHECK("nocopy refusals (bundle)", !u && !l, "unaligned pointer: %s; length not a page multiple: %s (the bundle can only import whole pages)", u ? "NON-NIL" : "nil", l ? "NON-NIL" : "nil");
#else
        AG_INFO("nocopy refusals", "Apple accepts an unaligned pointer (%s) and a non-page length (%s); the bundle refuses both (bundle-specific check on the PC)", u ? "non-nil" : "nil", l ? "non-nil" : "nil");
#endif
        id<MTLBuffer> ok = [dev newBufferWithBytesNoCopy:mem length:65536 options:MTLResourceStorageModeShared deallocator:nil];
#if defined(__x86_64__)
        { id<MTLTexture> nt = n_tex(dev, MTLPixelFormatA8Unorm, 32, 32, MTLTextureUsageShaderRead, MTLStorageModePrivate);
          AG_CHECK("unsupported pixel format -> nil (logged)", nt == nil, "A8Unorm texture: %s (the bundle logs one 'newTextureWithDescriptor NIL: pf ...' line)", nt ? "NON-NIL" : "nil"); }
#endif
        AG_CHECK("nocopy aligned after refusals", ok != nil, "a correct call still works");
        ok = nil; free(mem);
    }
    if (NRUN("tex")) {
        printf("mtlprobe: nocopy ---- tex\n");
        size_t len = 32768; uint8_t *mem = nc_alloc(len); memset(mem, 0xEE, len);
        uint8_t pat[64 * 64 * 4]; n_pattern(pat);
        for (int y = 0; y < 64; y++) memcpy(mem + y * 320, pat + y * 256, 256);   // bytesPerRow 320: 64 px of data + 16 px of padding per row
        id<MTLBuffer> b = [dev newBufferWithBytesNoCopy:mem length:len options:MTLResourceStorageModeShared deallocator:nil];
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:64 height:64 mipmapped:NO];
        td.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget; td.storageMode = MTLStorageModeShared;
        id<MTLTexture> t = [b newTextureWithDescriptor:td offset:0 bytesPerRow:320];
        AG_CHECK("nocopy texture create", t != nil, "class %s", t ? class_getName([(id)t class]) : "-");
        if (t) {
            NSData *g = ag_sample(dev, lib, q, t, "nocopy texture"); uint8_t exp[64 * 64 * 4];
            AG_CHECK("nocopy texture pixels", g && !memcmp(g.bytes, pat, sizeof pat), "64x64 BGRA8 over client memory with bytesPerRow 320 sampled exactly (padding skipped)");
            for (int y = 0; y < 64; y++) for (int x = 0; x < 256; x++) mem[y * 320 + x] = (uint8_t)(255 - pat[y * 256 + x]);   // CPU rewrite after first use
            for (int i = 0; i < 64 * 64 * 4; i++) exp[i] = (uint8_t)(255 - pat[i]);
            g = ag_sample(dev, lib, q, t, "nocopy texture rewrite");
            AG_CHECK("nocopy texture CPU rewrite", g && !memcmp(g.bytes, exp, sizeof exp), "a CPU rewrite of the client memory is sampled on the next command buffer");
        }
        // second texture in the same buffer at an offset, tight rows
        for (int i = 0; i < 64 * 64 * 4; i++) mem[16384 + i] = pat[(i * 5 + 1) % (64 * 64 * 4)];
        id<MTLTexture> t2 = [b newTextureWithDescriptor:td offset:16384 bytesPerRow:256];
        if (t2) { NSData *g = ag_sample(dev, lib, q, t2, "nocopy offset texture"); uint8_t e2[64 * 64 * 4]; for (int i = 0; i < 64 * 64 * 4; i++) e2[i] = pat[(i * 5 + 1) % (64 * 64 * 4)];
            AG_CHECK("nocopy texture at an offset", g && !memcmp(g.bytes, e2, sizeof e2), "texture at offset 16384, tight rows, exact"); }
        else AG_CHECK("nocopy texture at an offset", 0, "nil");
#if defined(__x86_64__)   // Apple's validation layer asserts on these (measured on an Apple-silicon Mac), so they are bundle-only
        id<MTLTexture> bad = [b newTextureWithDescriptor:td offset:0 bytesPerRow:100];
        id<MTLTexture> bad2 = [b newTextureWithDescriptor:td offset:len - 64 bytesPerRow:320];
        AG_CHECK("nocopy texture refusals (bundle)", !bad && !bad2, "bytesPerRow 100 (< row size): %s; offset past the end: %s", bad ? "non-nil" : "nil", bad2 ? "non-nil" : "nil");
#endif
        t = nil; t2 = nil; b = nil; free(mem);
    }
    if (NRUN("render")) {
        printf("mtlprobe: nocopy ---- render\n");
        size_t stride = 1088, len = 294912; uint8_t *mem = nc_alloc(len); memset(mem, 0xEE, len);
        id<MTLBuffer> b = [dev newBufferWithBytesNoCopy:mem length:len options:MTLResourceStorageModeShared deallocator:nil];
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:W height:H mipmapped:NO];
        td.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget; td.storageMode = MTLStorageModeShared;
        id<MTLTexture> t = [b newTextureWithDescriptor:td offset:0 bytesPerRow:stride];
        id<MTLRenderPipelineState> pso = ag_pso(dev, lib, NO);
        AG_CHECK("nocopy render target create", t && pso, "texture %d pipeline %d", t != nil, pso != nil);
        if (t && pso) {
            AgQuad qs[2] = { { 20, 20, 120, 120, { 1.0f, 0.0f, 0.2f, 1.0f } }, { 100, 100, 240, 230, { 0.0f, 1.0f, 0.4f, 1.0f } } };
            const uint8_t clr[4] = { 60, 40, 20, 255 };
            id<MTLCommandBuffer> cb = [q commandBuffer]; id<MTLRenderCommandEncoder> re = ag_begin(cb, t, YES, clr); [re setRenderPipelineState:pso];
            ag_draw_quad(re, &qs[0]); ag_draw_quad(re, &qs[1]); [re endEncoding];
            BOOL ok = n_run(cb, "nocopy render");
            static uint8_t exp[W * H * 4], got[W * H * 4]; ag_expect(exp, clr, qs, 2, NO);
            for (int y = 0; y < H; y++) memcpy(got + y * W * 4, mem + y * stride, W * 4);
            int pad = 0; for (int y = 0; y < H; y++) for (size_t x = W * 4; x < stride; x++) if (mem[y * stride + x] != 0xEE) pad++;
            int d = n_report_diff("nocopy render target", got, exp, W * H, 0, NULL, W, H);
            AG_CHECK("nocopy render into client memory", ok && d == 0 && pad == 0, "%d of %d pixels differ in the client memory (tolerance 0); %d padding bytes disturbed", d, W * H, pad);
        }
        t = nil; b = nil; free(mem);
    }
#if defined(__x86_64__)
    if (NRUN("spi")) {
        printf("mtlprobe: nocopy ---- spi (QuartzCore's forms, bundle only)\n");
        uint8_t pat[64 * 64 * 4]; n_pattern(pat);
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:64 height:64 mipmapped:NO];
        td.usage = MTLTextureUsageShaderRead; td.storageMode = MTLStorageModeShared;
        SEL s1 = sel_registerName("newTiledTextureWithBytesNoCopy:length:descriptor:offset:bytesPerRow:"), s2 = sel_registerName("newTiledTextureWithBytesNoCopy:length:deallocator:descriptor:offset:bytesPerRow:"), s3 = sel_registerName("newTextureWithBytesNoCopy:length:descriptor:deallocator:");
        uint8_t *m1 = nc_alloc(16384); memcpy(m1, pat, sizeof pat); int before = nc_freed;
        id t1 = (__bridge_transfer id)((void *(*)(id, SEL, void *, NSUInteger, id, NSUInteger, NSUInteger))objc_msgSend)(dev, s1, m1, 16384, td, 0, 256);   // new* returns +1
        AG_CHECK("spi newTiledTextureWithBytesNoCopy", t1 != nil, "class %s", t1 ? class_getName([t1 class]) : "-");
        if (t1) { NSData *g = ag_sample(dev, lib, q, t1, "spi tiled"); AG_CHECK("spi tiled pixels", g && !memcmp(g.bytes, pat, sizeof pat), "exact"); }
        uint8_t *m2 = nc_alloc(16384); memcpy(m2, pat, sizeof pat);
        @autoreleasepool {
            id t2 = (__bridge_transfer id)((void *(*)(id, SEL, void *, NSUInteger, id, id, NSUInteger, NSUInteger))objc_msgSend)(dev, s2, m2, 16384, nc_dealloc(), td, 0, 256);
            AG_CHECK("spi tiled with deallocator", t2 != nil, "class %s", t2 ? class_getName([t2 class]) : "-");
            if (t2) { NSData *g = ag_sample(dev, lib, q, t2, "spi tiled dealloc"); AG_CHECK("spi tiled with deallocator pixels", g && !memcmp(g.bytes, pat, sizeof pat), "exact"); }
            t2 = nil;
        }
        for (int w = 0; w < 1000 && nc_freed == before; w++) usleep(2000);   // the command buffer that sampled it is released by the completion thread
        AG_CHECK("spi deallocator", nc_freed == before + 1, "called %d time(s) after the texture was released (expected 1)", nc_freed - before);
        uint8_t *m3 = nc_alloc(16384); memcpy(m3, pat, sizeof pat);
        id t3 = (__bridge_transfer id)((void *(*)(id, SEL, void *, NSUInteger, id, id))objc_msgSend)(dev, s3, m3, 16384, td, nil);
        AG_CHECK("spi newTextureWithBytesNoCopy", t3 != nil, "class %s", t3 ? class_getName([t3 class]) : "-");
        if (t3) { NSData *g = ag_sample(dev, lib, q, t3, "spi plain"); AG_CHECK("spi plain pixels", g && !memcmp(g.bytes, pat, sizeof pat), "exact"); }
        id bad = (__bridge_transfer id)((void *(*)(id, SEL, void *, NSUInteger, id, NSUInteger, NSUInteger))objc_msgSend)(dev, s1, m1 + 64, 16384, td, 0, 256);
        AG_CHECK("spi unaligned pointer", bad == nil, "nil");
        t1 = nil; t3 = nil;
    }
#else
    if (NRUN("spi")) printf("mtlprobe: nocopy ---- spi: skipped on Apple silicon (private forms)\n");
#endif
    printf("mtlprobe: nocopy: %d check(s) FAILED\n", ag_fails);
    printf(ag_fails == 0 ? "mtlprobe: PASS nocopy\n" : "mtlprobe: FAIL nocopy\n");
    return ag_fails ? 1 : 0;
}

// ---------------------------------------------------------------------------------------------------------------
// m11h3: pixel-format coverage (`formats`) and 1D textures + CoreDisplay's GPU display pipe (`cdpipe`).
// Expected bytes are computed on the CPU from the Metal format definitions; every expectation was checked on an Apple-silicon Mac first.
// ---------------------------------------------------------------------------------------------------------------
typedef struct { MTLPixelFormat pf; const char *name; int bpp; MTLClearColor cc; uint8_t exp[16]; } FmtCase;
static uint16_t fc_half(float v) { __fp16 h = (__fp16)v; uint16_t u; memcpy(&u, &h, 2); return u; }
static int cmd_formats(const char *only, uint64_t rid, int haveRid) {
    id<MTLDevice> dev = pick_device(rid, haveRid); if (!dev) return 2;
    id<MTLCommandQueue> q = [dev newCommandQueue]; ag_fails = 0;
    #define FRUN(nm) (!only || !strcmp(only, nm))
    FmtCase cs[16]; int n = 0; memset(cs, 0, sizeof cs);
    // R32Float 0.375
    { FmtCase *c = &cs[n++]; c->pf = MTLPixelFormatR32Float; c->name = "R32Float"; c->bpp = 4; c->cc = MTLClearColorMake(0.375, 0, 0, 1); float f = 0.375f; memcpy(c->exp, &f, 4); }
    { FmtCase *c = &cs[n++]; c->pf = MTLPixelFormatRG32Float; c->name = "RG32Float"; c->bpp = 8; c->cc = MTLClearColorMake(0.375, 0.75, 0, 1); float f[2] = { 0.375f, 0.75f }; memcpy(c->exp, f, 8); }
    { FmtCase *c = &cs[n++]; c->pf = MTLPixelFormatR16Float; c->name = "R16Float"; c->bpp = 2; c->cc = MTLClearColorMake(0.375, 0, 0, 1); uint16_t h = fc_half(0.375f); memcpy(c->exp, &h, 2); }
    { FmtCase *c = &cs[n++]; c->pf = MTLPixelFormatRG16Float; c->name = "RG16Float"; c->bpp = 4; c->cc = MTLClearColorMake(0.375, 0.75, 0, 1); uint16_t h[2] = { fc_half(0.375f), fc_half(0.75f) }; memcpy(c->exp, h, 4); }
    { FmtCase *c = &cs[n++]; c->pf = MTLPixelFormatR16Unorm; c->name = "R16Unorm"; c->bpp = 2; c->cc = MTLClearColorMake(40000.0 / 65535, 0, 0, 1); uint16_t u = 40000; memcpy(c->exp, &u, 2); }
    { FmtCase *c = &cs[n++]; c->pf = MTLPixelFormatRG16Unorm; c->name = "RG16Unorm"; c->bpp = 4; c->cc = MTLClearColorMake(40000.0 / 65535, 12345.0 / 65535, 0, 1); uint16_t u[2] = { 40000, 12345 }; memcpy(c->exp, u, 4); }
    { FmtCase *c = &cs[n++]; c->pf = MTLPixelFormatRGBA16Unorm; c->name = "RGBA16Unorm"; c->bpp = 8; c->cc = MTLClearColorMake(40000.0 / 65535, 12345.0 / 65535, 54321.0 / 65535, 65535.0 / 65535); uint16_t u[4] = { 40000, 12345, 54321, 65535 }; memcpy(c->exp, u, 8); }
    // RGB10A2: red in the LOW bits; BGR10A2: blue in the low bits (alpha on top in both). r=300 g=700 b=1000 a=2 (of 3)
    { FmtCase *c = &cs[n++]; c->pf = MTLPixelFormatRGB10A2Unorm; c->name = "RGB10A2Unorm"; c->bpp = 4; c->cc = MTLClearColorMake(300.0 / 1023, 700.0 / 1023, 1000.0 / 1023, 2.0 / 3);
      uint32_t w = (2u << 30) | (1000u << 20) | (700u << 10) | 300u; memcpy(c->exp, &w, 4); }
    { FmtCase *c = &cs[n++]; c->pf = MTLPixelFormatBGR10A2Unorm; c->name = "BGR10A2Unorm"; c->bpp = 4; c->cc = MTLClearColorMake(300.0 / 1023, 700.0 / 1023, 1000.0 / 1023, 2.0 / 3);
      uint32_t w = (2u << 30) | (300u << 20) | (700u << 10) | 1000u; memcpy(c->exp, &w, 4); }
    // RG11B10Float: R 11 bits (5 exp, 6 mant) low, G 11 bits, B 10 bits (5 exp, 5 mant) on top. 0.5 = exp 14 mant 0, 0.25 = exp 13, 1.0 = exp 15
    { FmtCase *c = &cs[n++]; c->pf = MTLPixelFormatRG11B10Float; c->name = "RG11B10Float"; c->bpp = 4; c->cc = MTLClearColorMake(0.5, 0.25, 1.0, 1);
      uint32_t w = ((15u << 5) << 22) | ((13u << 6) << 11) | (14u << 6); memcpy(c->exp, &w, 4); }
    for (int i = 0; i < n; i++) {
        FmtCase *c = &cs[i];
        if (!FRUN(c->name) && !FRUN("all")) continue;
        printf("mtlprobe: formats ---- %s (pf %lu)\n", c->name, (unsigned long)c->pf);
        // (1) CPU round trip (Shared): upload a byte pattern with replaceRegion, read it back with getBytes (+ a GPU blit read-back): exact
        id<MTLTexture> t = n_tex(dev, c->pf, 32, 8, MTLTextureUsageShaderRead, MTLStorageModeShared);
        AG_CHECK("texture create (Shared, ShaderRead)", t != nil, "%s", c->name);
        if (t) {
            size_t rb = (size_t)32 * c->bpp; uint8_t *src = malloc(rb * 8), *dst = calloc(rb * 8, 1); for (size_t k = 0; k < rb * 8; k++) src[k] = (uint8_t)(k * 13 + 5);
            [t replaceRegion:MTLRegionMake2D(0, 0, 32, 8) mipmapLevel:0 withBytes:src bytesPerRow:rb];
            [t getBytes:dst bytesPerRow:rb fromRegion:MTLRegionMake2D(0, 0, 32, 8) mipmapLevel:0];
            AG_CHECK("replaceRegion/getBytes round trip", !memcmp(src, dst, rb * 8), "32x8 x %d bytes", c->bpp);
            NSData *gpu = n_readback(dev, q, t, 32, 8, c->bpp);
            AG_CHECK("GPU blit read-back", gpu && !memcmp(src, gpu.bytes, rb * 8), "same bytes through copyFromTexture:toBuffer:");
            free(src); free(dst);
        }
        // (2) render target: clear to a colour, read the raw bytes back; the format's packing must match the Metal definition exactly
        id<MTLTexture> rt = n_tex(dev, c->pf, 16, 16, MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead, MTLStorageModePrivate);
        AG_CHECK("render target create (Private)", rt != nil, "%s", c->name);
        if (rt) {
            id<MTLCommandBuffer> cb = [q commandBuffer]; MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
            rp.colorAttachments[0].texture = rt; rp.colorAttachments[0].loadAction = MTLLoadActionClear; rp.colorAttachments[0].storeAction = MTLStoreActionStore; rp.colorAttachments[0].clearColor = c->cc;
            id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:rp]; [re endEncoding];
            BOOL ok = n_run(cb, "formats clear");
            NSData *got = ok ? n_readback(dev, q, rt, 16, 16, c->bpp) : nil;
            int bad = 0; if (got) for (int p = 0; p < 256; p++) if (memcmp((const uint8_t *)got.bytes + p * c->bpp, c->exp, c->bpp)) bad++;
            char eh[40] = "", gh[40] = ""; for (int k = 0; k < c->bpp; k++) { snprintf(eh + k * 2, 4, "%02x", c->exp[k]); if (got) snprintf(gh + k * 2, 4, "%02x", ((const uint8_t *)got.bytes)[k]); }
            AG_CHECK("clear colour packing (raw bytes)", got && bad == 0, "%d of 256 pixels differ; expected %s got %s", got ? bad : -1, eh, gh);
        }
    }
    if (FRUN("pipe") || FRUN("all")) {   // the colour-attachment format gate of pipeline creation: every format of the table builds a pipeline when Apple's does
        printf("mtlprobe: formats ---- pipeline colour attachments\n");
        id<MTLLibrary> lib = n_lib(dev); if (!lib) return 2;
        for (int i = 0; i < n; i++) {
            MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
            pd.vertexFunction = [lib newFunctionWithName:@"blend_vs"]; pd.fragmentFunction = [lib newFunctionWithName:@"blend_fs"]; pd.colorAttachments[0].pixelFormat = cs[i].pf;
            NSError *e = nil; id pso = [dev newRenderPipelineStateWithDescriptor:pd error:&e];
            AG_CHECK("pipeline with this colour attachment", pso != nil, "%s%s%s", cs[i].name, e ? ": " : "", e ? e.localizedDescription.UTF8String : "");
        }
    }
    printf(ag_fails ? "mtlprobe: FAIL formats (%d)\n" : "mtlprobe: PASS formats\n", ag_fails);
    return ag_fails ? 1 : 0;
}

// ---- 1D textures (MTLTextureType1D / 1DArray) and CoreDisplay's GPUPass (the pipe WindowServer asked for: pf 55 type 1 16384x1 array 3) ----
static id<MTLTexture> t1_make(id<MTLDevice> dev, MTLTextureType tt, MTLPixelFormat pf, int w, int arr, MTLTextureUsage u, MTLStorageMode sm) {
    MTLTextureDescriptor *td = [MTLTextureDescriptor new]; td.textureType = tt; td.pixelFormat = pf; td.width = w; td.height = 1; td.depth = 1; td.arrayLength = arr; td.usage = u; td.storageMode = sm;
    return [dev newTextureWithDescriptor:td];
}
static int cmd_tex1d(const char *only, uint64_t rid, int haveRid) {
    id<MTLDevice> dev = pick_device(rid, haveRid); if (!dev) return 2;
    id<MTLCommandQueue> q = [dev newCommandQueue]; ag_fails = 0;
    #define TRUN(nm) (!only || !strcmp(only, nm))
    if (TRUN("basic")) {
        printf("mtlprobe: tex1d ---- basic\n");
        id<MTLTexture> t = t1_make(dev, MTLTextureType1D, MTLPixelFormatRGBA8Unorm, 100, 1, MTLTextureUsageShaderRead, MTLStorageModeShared);
        AG_CHECK("1D RGBA8 create", t != nil, "type %lu w %lu h %lu array %lu", t ? (unsigned long)t.textureType : 0, t ? (unsigned long)t.width : 0, t ? (unsigned long)t.height : 0, t ? (unsigned long)t.arrayLength : 0);
        if (t) {
            AG_CHECK("1D properties", t.textureType == MTLTextureType1D && t.width == 100 && t.height == 1 && t.arrayLength == 1, "textureType %lu", (unsigned long)t.textureType);
            uint8_t src[400], dst[400] = {0}; for (int i = 0; i < 400; i++) src[i] = (uint8_t)(i * 3 + 1);
            [t replaceRegion:MTLRegionMake1D(10, 50) mipmapLevel:0 slice:0 withBytes:src bytesPerRow:0 bytesPerImage:0];
            [t getBytes:dst bytesPerRow:0 bytesPerImage:0 fromRegion:MTLRegionMake1D(10, 50) mipmapLevel:0 slice:0];
            AG_CHECK("1D replaceRegion/getBytes (bytesPerRow 0 as Metal ignores it for 1D)", !memcmp(src, dst, 200), "50 texels at x=10");
        }
    }
    if (TRUN("array")) {
        printf("mtlprobe: tex1d ---- array (the WindowServer descriptor: R32Float 16384x1 array 3 ShaderRead Managed)\n");
        id<MTLTexture> t = t1_make(dev, MTLTextureType1DArray, MTLPixelFormatR32Float, 16384, 3, MTLTextureUsageShaderRead, N_MANAGED);
        AG_CHECK("1DArray R32Float 16384 x3 create", t != nil, "type %lu w %lu array %lu", t ? (unsigned long)t.textureType : 0, t ? (unsigned long)t.width : 0, t ? (unsigned long)t.arrayLength : 0);
        if (t) {
            AG_CHECK("1DArray properties", t.textureType == MTLTextureType1DArray && t.width == 16384 && t.height == 1 && t.arrayLength == 3 && t.pixelFormat == MTLPixelFormatR32Float, "ok");
            static float lut[3][16384], back[16384];
            for (int k = 0; k < 3; k++) for (int i = 0; i < 16384; i++) lut[k][i] = (float)(k + 1) * 0.25f + (float)i / 65536.0f;
            for (int k = 0; k < 3; k++) [t replaceRegion:MTLRegionMake1D(0, 16384) mipmapLevel:0 slice:k withBytes:lut[k] bytesPerRow:16384 * 4 bytesPerImage:0];
            int bad = 0;
            for (int k = 0; k < 3; k++) { memset(back, 0, sizeof back); [t getBytes:back bytesPerRow:16384 * 4 bytesPerImage:0 fromRegion:MTLRegionMake1D(0, 16384) mipmapLevel:0 slice:k]; if (memcmp(back, lut[k], sizeof back)) bad++; }
            AG_CHECK("per-slice replaceRegion/getBytes", bad == 0, "%d of 3 slices differ (each slice holds its own ramp)", bad);
            // GPU blit of slice 1 (x 1000..1999) to a buffer and back into slice 2
            id<MTLBuffer> b = [dev newBufferWithLength:16384 * 4 options:MTLResourceStorageModeShared]; memset(b.contents, 0, 16384 * 4);
            id<MTLCommandBuffer> cb = [q commandBuffer]; id<MTLBlitCommandEncoder> be = [cb blitCommandEncoder];
            [be copyFromTexture:t sourceSlice:1 sourceLevel:0 sourceOrigin:MTLOriginMake(1000, 0, 0) sourceSize:MTLSizeMake(1000, 1, 1) toBuffer:b destinationOffset:0 destinationBytesPerRow:4000 destinationBytesPerImage:4000];
            [be copyFromBuffer:b sourceOffset:0 sourceBytesPerRow:4000 sourceBytesPerImage:4000 sourceSize:MTLSizeMake(1000, 1, 1) toTexture:t destinationSlice:2 destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
            [be endEncoding];
            BOOL ok = n_run(cb, "tex1d blit");
            AG_CHECK("blit slice 1 -> buffer exact", ok && !memcmp(b.contents, &lut[1][1000], 4000), "1000 texels");
            memset(back, 0, sizeof back); [t getBytes:back bytesPerRow:16384 * 4 bytesPerImage:0 fromRegion:MTLRegionMake1D(0, 1000) mipmapLevel:0 slice:2];
            AG_CHECK("blit buffer -> slice 2 exact", !memcmp(back, &lut[1][1000], 4000), "slice 2 x 0..999 now holds slice 1's texels 1000..1999");
            [t getBytes:back bytesPerRow:16384 * 4 bytesPerImage:0 fromRegion:MTLRegionMake1D(1000, 1000) mipmapLevel:0 slice:2];
            AG_CHECK("slice 2 beyond the blit untouched", !memcmp(back, &lut[2][1000], 4000), "texels 1000..1999 unchanged");
        }
    }
    printf(ag_fails ? "mtlprobe: FAIL tex1d (%d)\n" : "mtlprobe: PASS tex1d\n", ag_fails);
    return ag_fails ? 1 : 0;
}

// cdpipe: CoreDisplay's ViewportToNDC + GPUPass. (1) pipelines for the colour formats; (2) a draw with flags 0 (pure copy) and flags 4 (the 1D-array LUT stage: per channel
// out_k = lut_k(scale * c_k + bias), sampled with linear filtering) into BGRA8.
static int cd_draw(id<MTLDevice> dev, id<MTLCommandQueue> q, id<MTLRenderPipelineState> pso, id<MTLTexture> src, id<MTLTexture> lut, uint32_t flags, float scale, float bias, id<MTLTexture> tgt) {
    id<MTLBuffer> b0 = [dev newBufferWithLength:256 options:MTLResourceStorageModeShared], b1 = [dev newBufferWithLength:256 options:MTLResourceStorageModeShared];
    float pos[8] = { -1, 1,  1, 1,  -1, -1,  1, -1 }, tc[8] = { 0, 0,  64, 0,  0, 64,  64, 64 };
    memset(b0.contents, 0x7F, 256); memset(b1.contents, 0x7F, 256); memcpy((uint8_t *)b0.contents + 16, pos, sizeof pos); memcpy((uint8_t *)b1.contents + 32, tc, sizeof tc);
    float id4[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 }; float z[32] = {0};
    MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new]; sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterNearest; sd.sAddressMode = sd.tAddressMode = MTLSamplerAddressModeClampToEdge; sd.normalizedCoordinates = NO;
    id<MTLSamplerState> smp = [dev newSamplerStateWithDescriptor:sd];
    id<MTLCommandBuffer> cb = [q commandBuffer]; MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = tgt; rp.colorAttachments[0].loadAction = MTLLoadActionClear; rp.colorAttachments[0].storeAction = MTLStoreActionStore; rp.colorAttachments[0].clearColor = MTLClearColorMake(1, 0, 1, 1);
    id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:rp];
    [re setRenderPipelineState:pso];
    [re setVertexBuffer:b0 offset:16 atIndex:0]; [re setVertexBuffer:b1 offset:32 atIndex:1]; [re setVertexBytes:id4 length:sizeof id4 atIndex:2];
    [re setFragmentBytes:&flags length:4 atIndex:0]; [re setFragmentBytes:z length:128 atIndex:1]; [re setFragmentBytes:id4 length:sizeof id4 atIndex:2];
    [re setFragmentBytes:&scale length:4 atIndex:3]; [re setFragmentBytes:&bias length:4 atIndex:4]; [re setFragmentBytes:id4 length:sizeof id4 atIndex:5];
    [re setFragmentTexture:src atIndex:0]; [re setFragmentTexture:lut atIndex:1]; [re setFragmentSamplerState:smp atIndex:0];
    [re drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4]; [re endEncoding];
    return n_run(cb, "cdpipe draw");
}
static int cmd_cdpipe(const char *only, uint64_t rid, int haveRid) {
    id<MTLDevice> dev = pick_device(rid, haveRid); if (!dev) return 2;
    NSError *err = nil; ag_fails = 0;
    id<MTLLibrary> lib = [dev newLibraryWithURL:[NSURL fileURLWithPath:@"/System/Library/Frameworks/CoreDisplay.framework/Versions/A/Resources/default.metallib"] error:&err]; perr("newLibraryWithURL", err);
    // GPUPass is specialised by two uint function constants (availableFeatures idx 0, optionalFeatures idx 1); the AIR WindowServer dumped contains every stage, WindowServer used 0xFFFFFFFF for both (its dumped AIR carries i32 -1), so does this probe.
    MTLFunctionConstantValues *cv = [MTLFunctionConstantValues new]; unsigned fv = 0xFFFFFFFFu; [cv setConstantValue:&fv type:MTLDataTypeUInt atIndex:0]; [cv setConstantValue:&fv type:MTLDataTypeUInt atIndex:1];
    id<MTLFunction> vs = lib ? [lib newFunctionWithName:@"ViewportToNDC"] : nil, fs = lib ? [lib newFunctionWithName:@"GPUPass" constantValues:cv error:&err] : nil; perr("GPUPass specialisation", err);
    if (!vs || !fs) { printf("mtlprobe: FAIL CoreDisplay ViewportToNDC/GPUPass missing (vs %s fs %s)\n", vs ? "ok" : "NIL", fs ? "ok" : "NIL"); return 2; }
    #define CRUN(nm) (!only || !strcmp(only, nm))
    static const struct { MTLPixelFormat pf; const char *nm; } fm[] = { { MTLPixelFormatBGRA8Unorm, "BGRA8Unorm" }, { MTLPixelFormatBGRA8Unorm_sRGB, "BGRA8Unorm_sRGB" }, { MTLPixelFormatRGBA8Unorm, "RGBA8Unorm" },
        { MTLPixelFormatRGB10A2Unorm, "RGB10A2Unorm" }, { MTLPixelFormatBGR10A2Unorm, "BGR10A2Unorm" }, { MTLPixelFormatRGBA16Float, "RGBA16Float" }, { MTLPixelFormatRGBA32Float, "RGBA32Float" },
        { MTLPixelFormatRGBA16Unorm, "RGBA16Unorm" } };
    id<MTLRenderPipelineState> psoBGRA = nil;
    if (CRUN("pipe")) {
        printf("mtlprobe: cdpipe ---- GPUPass pipelines per colour format\n");
        for (size_t i = 0; i < sizeof fm / sizeof *fm; i++) {
            MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new]; pd.vertexFunction = vs; pd.fragmentFunction = fs; pd.colorAttachments[0].pixelFormat = fm[i].pf;
            err = nil; id<MTLRenderPipelineState> p = [dev newRenderPipelineStateWithDescriptor:pd error:&err];
            AG_CHECK("ViewportToNDC + GPUPass pipeline", p != nil, "%s%s%s", fm[i].nm, err ? ": " : "", err ? err.localizedDescription.UTF8String : "");
            if (fm[i].pf == MTLPixelFormatBGRA8Unorm) psoBGRA = p;
        }
    }
    if (CRUN("draw")) {
        printf("mtlprobe: cdpipe ---- draws into BGRA8 (flags 0 copy; flags 4 LUT)\n");
        if (!psoBGRA) { MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new]; pd.vertexFunction = vs; pd.fragmentFunction = fs; pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm; err = nil; psoBGRA = [dev newRenderPipelineStateWithDescriptor:pd error:&err]; perr("pipeline", err); }
        if (!psoBGRA) { printf("mtlprobe: FAIL pipeline\n"); return 2; }
        id<MTLCommandQueue> q = [dev newCommandQueue];
        id<MTLTexture> src = n_source_texture(dev, MTLPixelFormatBGRA8Unorm, N_MANAGED);
        id<MTLTexture> lut = t1_make(dev, MTLTextureType1DArray, MTLPixelFormatR32Float, 16384, 3, MTLTextureUsageShaderRead, N_MANAGED);
        id<MTLTexture> tgt = n_tex(dev, MTLPixelFormatBGRA8Unorm, 64, 64, MTLTextureUsageRenderTarget, MTLStorageModePrivate);
        if (!src || !lut || !tgt) { printf("mtlprobe: FAIL alloc (src %d lut %d tgt %d)\n", !!src, !!lut, !!tgt); return 2; }
        static float ramp[3][16384]; static const float g[3] = { 1.0f, 0.75f, 0.5f };
        for (int k = 0; k < 3; k++) { for (int i = 0; i < 16384; i++) ramp[k][i] = g[k] * ((float)i + 0.5f) / 16384.0f; [lut replaceRegion:MTLRegionMake1D(0, 16384) mipmapLevel:0 slice:k withBytes:ramp[k] bytesPerRow:16384 * 4 bytesPerImage:0]; }
        uint8_t pat[64 * 64 * 4]; n_pattern(pat);
        static uint8_t exp[64 * 64 * 4];
        // flags 0: a pure copy (as TextureCopy)
        BOOL ok = cd_draw(dev, q, psoBGRA, src, lut, 0, 1, 0, tgt); NSData *got = ok ? n_readback(dev, q, tgt, 64, 64, 4) : nil;
        if (got) { for (int i = 0; i < 64 * 64; i++) { exp[i*4] = pat[i*4+2]; exp[i*4+1] = pat[i*4+1]; exp[i*4+2] = pat[i*4]; exp[i*4+3] = pat[i*4+3]; }
            int d = n_report_diff("cdpipe GPUPass flags 0 (copy)", got.bytes, exp, 64 * 64, 0, NULL, 64, 64); AG_CHECK("GPUPass flags 0 copy (exact)", d == 0, "%d pixels differ", d); }
        else AG_CHECK("GPUPass flags 0 copy", 0, "draw/readback failed");
        // flags 4: out_k = g_k * (0.25 + 0.5 * c_k) (linear ramps sampled at normalised coordinate scale*c+bias), alpha from the source; BGRA byte order; tolerance 1 LSB
        ok = cd_draw(dev, q, psoBGRA, src, lut, 4, 0.5f, 0.25f, tgt); got = ok ? n_readback(dev, q, tgt, 64, 64, 4) : nil;
        if (got) { for (int i = 0; i < 64 * 64; i++) { for (int k = 0; k < 3; k++) { float c = pat[i*4+k] / 255.0f, v = g[k] * (0.25f + 0.5f * c); exp[i*4 + (2 - k)] = (uint8_t)lrintf(v * 255.0f); } exp[i*4+3] = pat[i*4+3]; }
            int d = n_report_diff("cdpipe GPUPass flags 4 (LUT)", got.bytes, exp, 64 * 64, 1, NULL, 64, 64); AG_CHECK("GPUPass flags 4 through the 1DArray R32Float LUT (within 1 LSB)", d == 0, "%d pixels differ", d); }
        else AG_CHECK("GPUPass flags 4 LUT", 0, "draw/readback failed");
    }
    printf(ag_fails ? "mtlprobe: FAIL cdpipe (%d)\n" : "mtlprobe: PASS cdpipe\n", ag_fails);
    return ag_fails ? 1 : 0;
}



// ---------------------------------------------------------------------------------------------------------------
// gaps2 (m11h9, NATIVE-S4-METAL-GAPS.md section 10): the textures WindowServer asked for in the first Metal-compositor run:
// g1 IOSurface textures of BGR10A2Unorm (pf 94, the crash) and RGBA16Float, g2 A8Unorm, g3 3D textures, g4 mipmapped 2D textures (+ generateMipmaps,
// render into a level), refuse (bundle refusals, PC only). Every expectation was checked on an Apple-silicon Mac first (mtlprobe-arm64 gaps2: real Metal).
// ---------------------------------------------------------------------------------------------------------------
static id<MTLRenderPipelineState> g2_pso(id<MTLDevice> dev, id<MTLLibrary> lib, const char *fs, MTLPixelFormat pf) {
    MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
    pd.vertexFunction = [lib newFunctionWithName:@"g2_vs"]; pd.fragmentFunction = [lib newFunctionWithName:@(fs)]; pd.colorAttachments[0].pixelFormat = pf;
    NSError *e = nil; id<MTLRenderPipelineState> p = [dev newRenderPipelineStateWithDescriptor:pd error:&e]; perr("g2 pso", e); return p;
}
static id<MTLSamplerState> g2_smp(id<MTLDevice> dev, BOOL linear, int mipf) {
    MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new];
    sd.minFilter = sd.magFilter = linear ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
    sd.mipFilter = mipf == 0 ? MTLSamplerMipFilterNotMipmapped : mipf == 1 ? MTLSamplerMipFilterNearest : MTLSamplerMipFilterLinear;
    sd.sAddressMode = sd.tAddressMode = sd.rAddressMode = MTLSamplerAddressModeClampToEdge;
    return [dev newSamplerStateWithDescriptor:sd];
}
// NDC rect for a pixel rectangle (x0,y0)-(x1,y1) (y down) of a w x h target.
static void g2_rect(float r[4], int x0, int y0, int x1, int y1, int w, int h) { r[0] = x0 * 2.0f / w - 1.0f; r[1] = 1.0f - y1 * 2.0f / h; r[2] = (x1 - x0) * 2.0f / w; r[3] = (y1 - y0) * 2.0f / h; }
// Samples `src` with fragment function `fs` into a fresh Private tw x th target of `tpf` (cleared to 0), over the whole target; returns the raw target bytes.
static NSData *g2_sample_into(id<MTLDevice> dev, id<MTLCommandQueue> q, id<MTLLibrary> lib, const char *fs, id<MTLTexture> src, float param, BOOL linear, int mipf,
                              MTLPixelFormat tpf, int tw, int th, int tbpp) {
    id<MTLRenderPipelineState> pso = g2_pso(dev, lib, fs, tpf); id<MTLSamplerState> ss = g2_smp(dev, linear, mipf);
    id<MTLTexture> tgt = n_tex(dev, tpf, tw, th, MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead, MTLStorageModePrivate);
    if (!pso || !ss || !tgt || !src) { printf("mtlprobe: g2 sample: alloc failed (pso %d smp %d tgt %d src %d)\n", !!pso, !!ss, !!tgt, !!src); return nil; }
    id<MTLCommandBuffer> cb = [q commandBuffer]; MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = tgt; rp.colorAttachments[0].loadAction = MTLLoadActionClear; rp.colorAttachments[0].storeAction = MTLStoreActionStore; rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);
    id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:rp];
    float rect[4] = { -1, -1, 2, 2 };
    [re setRenderPipelineState:pso]; [re setVertexBytes:rect length:sizeof rect atIndex:0];
    [re setFragmentTexture:src atIndex:0]; [re setFragmentSamplerState:ss atIndex:0]; [re setFragmentBytes:&param length:sizeof param atIndex:0];
    [re drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4]; [re endEncoding];
    if (!n_run(cb, "g2 sample")) return nil;
    return n_readback(dev, q, tgt, tw, th, tbpp);
}
static NSData *g2_sample(id<MTLDevice> dev, id<MTLCommandQueue> q, id<MTLLibrary> lib, const char *fs, id<MTLTexture> src, float param, BOOL linear, int mipf) {
    return g2_sample_into(dev, q, lib, fs, src, param, linear, mipf, MTLPixelFormatBGRA8Unorm, 16, 16, 4);
}
// Compares a 16x16 BGRA8 result with a per-pixel expectation (B,G,R,A bytes); returns 1 on failure.
static int g2_cmp(const char *tag, NSData *got, const uint8_t *exp, int tol) {
    if (!got) { AG_CHECK(tag, 0, "no result"); return 1; }
    int d = n_report_diff(tag, got.bytes, exp, 256, tol, NULL, 16, 16); AG_CHECK(tag, d == 0, "%d of 256 pixels differ (tolerance %d LSB)", d, tol); return d != 0;
}
static void g2_solid(uint8_t *exp, const uint8_t bgra[4]) { for (int i = 0; i < 256; i++) memcpy(exp + i * 4, bgra, 4); }
// One render pass into `tex` (level `level`): optional clear, optional flat quad (colour c over the pixel rectangle of a tw x th level).
static BOOL g2_flat(id<MTLDevice> dev, id<MTLCommandQueue> q, id<MTLLibrary> lib, id<MTLTexture> tex, NSUInteger level, BOOL clr, MTLClearColor cc, const float *c, int x0, int y0, int x1, int y1, int tw, int th) {
    id<MTLRenderPipelineState> pso = c ? g2_pso(dev, lib, "g2_flat", tex.pixelFormat) : nil; if (c && !pso) return NO;
    id<MTLCommandBuffer> cb = [q commandBuffer]; MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = tex; rp.colorAttachments[0].level = level; rp.colorAttachments[0].loadAction = clr ? MTLLoadActionClear : MTLLoadActionLoad;
    rp.colorAttachments[0].storeAction = MTLStoreActionStore; rp.colorAttachments[0].clearColor = cc;
    id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:rp];
    if (c) { float rect[4]; g2_rect(rect, x0, y0, x1, y1, tw, th); [re setRenderPipelineState:pso]; [re setVertexBytes:rect length:sizeof rect atIndex:0]; [re setFragmentBytes:c length:16 atIndex:0];
             [re drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4]; }
    [re endEncoding];
    return n_run(cb, "g2 flat");
}

// ---- g1: IOSurface textures of pixel formats other than BGRA8/RGBA8 ----
static IOSurfaceRef io_make2(int w, int h, int bpe, uint32_t fourcc) {
    int32_t v[4] = { w, h, bpe, (int32_t)fourcc };
    CFNumberRef nw = CFNumberCreate(NULL, kCFNumberSInt32Type, &v[0]), nh = CFNumberCreate(NULL, kCFNumberSInt32Type, &v[1]), nb = CFNumberCreate(NULL, kCFNumberSInt32Type, &v[2]), np = CFNumberCreate(NULL, kCFNumberSInt32Type, &v[3]);
    CFMutableDictionaryRef pr = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(pr, kIOSurfaceWidth, nw); CFDictionarySetValue(pr, kIOSurfaceHeight, nh); CFDictionarySetValue(pr, kIOSurfaceBytesPerElement, nb); CFDictionarySetValue(pr, kIOSurfacePixelFormat, np);
    IOSurfaceRef s = IOSurfaceCreate(pr); CFRelease(pr); CFRelease(nw); CFRelease(nh); CFRelease(nb); CFRelease(np); return s;
}
static id<MTLTexture> io_tex2(id<MTLDevice> dev, IOSurfaceRef s, MTLPixelFormat pf, int w, int h, MTLTextureUsage u) {
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:pf width:w height:h mipmapped:NO]; td.usage = u; td.storageMode = N_MANAGED;
    return [dev newTextureWithDescriptor:td iosurface:s plane:0];
}
// CPU patterns. bpp 4 = BGR10A2 words (blue in the LOW bits); bpp 8 = RGBA16Float halves (every value exact in half).
static void g1_pixel(uint8_t *p, int bpp, int x, int y) {
    if (bpp == 4) { uint32_t b = (uint32_t)(x * 61 + y * 17) & 1023, g = (uint32_t)(x * 7 + y * 91 + 300) & 1023, r = (uint32_t)(x * 29 + y * 5 + 600) & 1023, a = (uint32_t)(x + y) & 3;
                    uint32_t w = (a << 30) | (r << 20) | (g << 10) | b; memcpy(p, &w, 4); }
    else { uint16_t h[4] = { fc_half(x / 16.0f), fc_half(y / 16.0f), fc_half((x + y) / 32.0f - 0.5f), fc_half(1.0f - x / 64.0f) }; memcpy(p, h, 8); }
}
static int g1_rows_differ(IOSurfaceRef s, int w, int h, int bpp, const uint8_t *clearPx, const uint8_t *drawPx, int x0, int y0, int x1, int y1) {
    int bad = 0; IOSurfaceLock(s, kIOSurfaceLockReadOnly, NULL); const uint8_t *b = IOSurfaceGetBaseAddress(s); size_t bpr = IOSurfaceGetBytesPerRow(s);
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) { BOOL in = x >= x0 && x < x1 && y >= y0 && y < y1; if (memcmp(b + y * bpr + x * bpp, in ? drawPx : clearPx, bpp)) bad++; }
    IOSurfaceUnlock(s, kIOSurfaceLockReadOnly, NULL); return bad;
}
static void g1_case(id<MTLDevice> dev, id<MTLCommandQueue> q, id<MTLLibrary> lib, const char *name, MTLPixelFormat pf, uint32_t fourcc, int bpp) {
    printf("mtlprobe: gaps2 g1 ---- %s (pf %lu, IOSurface %d bytes/element)\n", name, (unsigned long)pf, bpp);
    // (a) sample a 16x16 IOSurface texture into a Private render target of the same format: the raw target bytes must equal the CPU-written surface
    IOSurfaceRef s = io_make2(16, 16, bpp, fourcc);
    AG_CHECK("IOSurface create", s != NULL, "16x16 %d bytes/element", bpp);
    if (!s) return;
    IOSurfaceLock(s, 0, NULL); { uint8_t *b = IOSurfaceGetBaseAddress(s); size_t bpr = IOSurfaceGetBytesPerRow(s); for (int y = 0; y < 16; y++) for (int x = 0; x < 16; x++) g1_pixel(b + y * bpr + x * bpp, bpp, x, y); } IOSurfaceUnlock(s, 0, NULL);
    id<MTLTexture> t = io_tex2(dev, s, pf, 16, 16, MTLTextureUsageShaderRead);
    AG_CHECK("newTextureWithDescriptor:iosurface:plane: non-nil", t != nil, "%s 16x16, bytesPerRow %zu", name, IOSurfaceGetBytesPerRow(s));
    if (t) {
        AG_CHECK("texture.iosurface == surface, pixelFormat", t.iosurface == s && t.pixelFormat == pf, "pf %lu", (unsigned long)t.pixelFormat);
        NSData *got = g2_sample_into(dev, q, lib, "g2_tex2d", t, 0, NO, 0, pf, 16, 16, bpp);
        uint8_t exp[16 * 16 * 8]; for (int y = 0; y < 16; y++) for (int x = 0; x < 16; x++) g1_pixel(exp + (y * 16 + x) * bpp, bpp, x, y);
        int bad = 0; if (got) for (int i = 0; i < 256; i++) if (memcmp((const uint8_t *)got.bytes + i * bpp, exp + i * bpp, bpp)) bad++;
        AG_CHECK("sampled into a same-format render target (raw bytes)", got && bad == 0, "%d of 256 pixels differ", got ? bad : -1);
    }
    // (b) render into a 64x64 surface (clear + one quad), CPU read-back of the surface memory; poisoned first
    IOSurfaceRef s2 = io_make2(64, 64, bpp, fourcc);
    id<MTLTexture> t2 = s2 ? io_tex2(dev, s2, pf, 64, 64, MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead) : nil;
    AG_CHECK("render-target IOSurface texture non-nil", t2 != nil, "64x64 %s", name);
    if (t2) {
        memset(IOSurfaceGetBaseAddress(s2), 0xEE, IOSurfaceGetAllocSize(s2));
        uint8_t cpx[8], dpx[8]; MTLClearColor cc; float dc[4];
        if (bpp == 4) { uint32_t cw = (2u << 30) | (300u << 20) | (700u << 10) | 1000u, dw = (3u << 30) | (100u << 20) | (200u << 10) | 900u; memcpy(cpx, &cw, 4); memcpy(dpx, &dw, 4);
                        cc = MTLClearColorMake(300.0 / 1023, 700.0 / 1023, 1000.0 / 1023, 2.0 / 3); dc[0] = 100.0f / 1023; dc[1] = 200.0f / 1023; dc[2] = 900.0f / 1023; dc[3] = 1.0f; }
        else { uint16_t ch[4] = { fc_half(0.25f), fc_half(0.5f), fc_half(0.75f), fc_half(1.0f) }, dh[4] = { fc_half(2.0f), fc_half(-1.0f), fc_half(0.125f), fc_half(0.5f) }; memcpy(cpx, ch, 8); memcpy(dpx, dh, 8);
               cc = MTLClearColorMake(0.25, 0.5, 0.75, 1.0); dc[0] = 2.0f; dc[1] = -1.0f; dc[2] = 0.125f; dc[3] = 0.5f; }
        BOOL ok = g2_flat(dev, q, lib, t2, 0, YES, cc, dc, 16, 8, 48, 40, 64, 64);
        int bad = ok ? g1_rows_differ(s2, 64, 64, bpp, cpx, dpx, 16, 8, 48, 40) : -1;
        AG_CHECK("render into the IOSurface: clear + quad, CPU read of the surface (raw)", ok && bad == 0, "%d of 4096 pixels differ", bad);
        // a second pass LOADs the surface contents (CPU poked first) and draws a quad over them
        IOSurfaceLock(s2, 0, NULL); { uint8_t *b = IOSurfaceGetBaseAddress(s2); size_t bpr = IOSurfaceGetBytesPerRow(s2); for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) memcpy(b + y * bpr + x * bpp, dpx, bpp); } IOSurfaceUnlock(s2, 0, NULL);
        ok = g2_flat(dev, q, lib, t2, 0, NO, MTLClearColorMake(0, 0, 0, 0), dc, 56, 56, 64, 64, 64, 64);
        int bad2 = 0; if (ok) { IOSurfaceLock(s2, kIOSurfaceLockReadOnly, NULL); const uint8_t *b = IOSurfaceGetBaseAddress(s2); size_t bpr = IOSurfaceGetBytesPerRow(s2);
            for (int y = 0; y < 64; y++) for (int x = 0; x < 64; x++) { BOOL qd = x >= 56 && y >= 56, pk = x < 4 && y < 4, rc = x >= 16 && x < 48 && y >= 8 && y < 40;
                const uint8_t *e = (qd || pk || rc) ? dpx : cpx; if (memcmp(b + y * bpr + x * bpp, e, bpp)) bad2++; }
            IOSurfaceUnlock(s2, kIOSurfaceLockReadOnly, NULL); } else bad2 = -1;
        AG_CHECK("second pass LOADs the CPU-modified contents", ok && bad2 == 0, "%d of 4096 pixels differ", bad2);
    }
    // (c) the display-size surface of the compositor (2560x1440): create, clear + quad, CPU check of every pixel
    IOSurfaceRef s3 = io_make2(2560, 1440, bpp, fourcc);
    id<MTLTexture> t3 = s3 ? io_tex2(dev, s3, pf, 2560, 1440, MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead) : nil;
    AG_CHECK("2560x1440 render-target IOSurface texture non-nil (the crashing call)", t3 != nil, "%s bytesPerRow %zu allocSize %zu", name, s3 ? IOSurfaceGetBytesPerRow(s3) : 0, s3 ? IOSurfaceGetAllocSize(s3) : 0);
    if (t3) {
        memset(IOSurfaceGetBaseAddress(s3), 0xEE, IOSurfaceGetAllocSize(s3));
        uint8_t cpx[8], dpx[8]; MTLClearColor cc; float dc[4];
        if (bpp == 4) { uint32_t cw = (2u << 30) | (300u << 20) | (700u << 10) | 1000u, dw = (3u << 30) | (100u << 20) | (200u << 10) | 900u; memcpy(cpx, &cw, 4); memcpy(dpx, &dw, 4);
                        cc = MTLClearColorMake(300.0 / 1023, 700.0 / 1023, 1000.0 / 1023, 2.0 / 3); dc[0] = 100.0f / 1023; dc[1] = 200.0f / 1023; dc[2] = 900.0f / 1023; dc[3] = 1.0f; }
        else { uint16_t ch[4] = { fc_half(0.25f), fc_half(0.5f), fc_half(0.75f), fc_half(1.0f) }, dh[4] = { fc_half(2.0f), fc_half(-1.0f), fc_half(0.125f), fc_half(0.5f) }; memcpy(cpx, ch, 8); memcpy(dpx, dh, 8);
               cc = MTLClearColorMake(0.25, 0.5, 0.75, 1.0); dc[0] = 2.0f; dc[1] = -1.0f; dc[2] = 0.125f; dc[3] = 0.5f; }
        BOOL ok = g2_flat(dev, q, lib, t3, 0, YES, cc, dc, 100, 200, 1500, 900, 2560, 1440);
        int bad = ok ? g1_rows_differ(s3, 2560, 1440, bpp, cpx, dpx, 100, 200, 1500, 900) : -1;
        AG_CHECK("2560x1440: clear + quad, every pixel CPU-checked", ok && bad == 0, "%d of %d pixels differ", bad, 2560 * 1440);
    }
}
static void g1_run(id<MTLDevice> dev, id<MTLCommandQueue> q, id<MTLLibrary> lib) {
    g1_case(dev, q, lib, "BGR10A2Unorm", MTLPixelFormatBGR10A2Unorm, 'l10r', 4);
    g1_case(dev, q, lib, "RGBA16Float", MTLPixelFormatRGBA16Float, 'RGhA', 8);
}

// ---- g2: A8Unorm ----
static void g2_a8(id<MTLDevice> dev, id<MTLCommandQueue> q, id<MTLLibrary> lib) {
    printf("mtlprobe: gaps2 g2 ---- A8Unorm\n");
    uint8_t pat[16 * 16], got8[16 * 16], exp[256 * 4];
    for (int y = 0; y < 16; y++) for (int x = 0; x < 16; x++) pat[y * 16 + x] = (uint8_t)(x * 16 + y * 3 + 5);
    for (int i = 0; i < 256; i++) { exp[i * 4] = 0; exp[i * 4 + 1] = 0; exp[i * 4 + 2] = 0; exp[i * 4 + 3] = pat[i]; }   // sampling an A8 texture returns (0,0,0,a)
    id<MTLTexture> t = n_tex(dev, MTLPixelFormatA8Unorm, 16, 16, MTLTextureUsageShaderRead, N_MANAGED);
    AG_CHECK("A8Unorm 16x16 ShaderRead create", t != nil, "pf %lu", t ? (unsigned long)t.pixelFormat : 0);
    if (t) {
        AG_CHECK("properties", t.pixelFormat == MTLPixelFormatA8Unorm && t.width == 16 && t.height == 16 && t.textureType == MTLTextureType2D, "pf %lu", (unsigned long)t.pixelFormat);
        [t replaceRegion:MTLRegionMake2D(0, 0, 16, 16) mipmapLevel:0 withBytes:pat bytesPerRow:16];
        memset(got8, 0, sizeof got8); [t getBytes:got8 bytesPerRow:16 fromRegion:MTLRegionMake2D(0, 0, 16, 16) mipmapLevel:0];
        AG_CHECK("replaceRegion/getBytes round trip", !memcmp(pat, got8, 256), "16x16 bytes");
        g2_cmp("sampled (0,0,0,a) into BGRA8", g2_sample(dev, q, lib, "g2_tex2d", t, 0, NO, 0), exp, 0);
        uint8_t part[15]; for (int i = 0; i < 15; i++) part[i] = (uint8_t)(200 + i);
        [t replaceRegion:MTLRegionMake2D(4, 2, 5, 3) mipmapLevel:0 withBytes:part bytesPerRow:5];
        uint8_t model[256]; memcpy(model, pat, 256); for (int y = 0; y < 3; y++) for (int x = 0; x < 5; x++) model[(2 + y) * 16 + 4 + x] = part[y * 5 + x];
        memset(got8, 0, sizeof got8); [t getBytes:got8 bytesPerRow:16 fromRegion:MTLRegionMake2D(0, 0, 16, 16) mipmapLevel:0];
        AG_CHECK("partial replaceRegion 5x3 at (4,2) leaves the rest", !memcmp(model, got8, 256), "bytes");
    }
    // Private A8: upload through a blit, read back through a blit, sample
    id<MTLTexture> tp = n_tex(dev, MTLPixelFormatA8Unorm, 16, 16, MTLTextureUsageShaderRead, MTLStorageModePrivate);
    AG_CHECK("A8 Private create", tp != nil, "");
    if (tp) {
        id<MTLBuffer> b1 = [dev newBufferWithLength:256 options:MTLResourceStorageModeShared], b2 = [dev newBufferWithLength:256 options:MTLResourceStorageModeShared];
        memcpy(b1.contents, pat, 256); memset(b2.contents, 0xEE, 256);
        id<MTLCommandBuffer> cb = [q commandBuffer]; id<MTLBlitCommandEncoder> be = [cb blitCommandEncoder];
        [be copyFromBuffer:b1 sourceOffset:0 sourceBytesPerRow:16 sourceBytesPerImage:256 sourceSize:MTLSizeMake(16, 16, 1) toTexture:tp destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
        [be copyFromTexture:tp sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(16, 16, 1) toBuffer:b2 destinationOffset:0 destinationBytesPerRow:16 destinationBytesPerImage:256];
        [be endEncoding]; BOOL ok = n_run(cb, "a8 blit");
        AG_CHECK("Private: buffer -> texture -> buffer blit round trip", ok && !memcmp(b2.contents, pat, 256), "raw A8 bytes");
        g2_cmp("Private sampled (0,0,0,a)", g2_sample(dev, q, lib, "g2_tex2d", tp, 0, NO, 0), exp, 0);
    }
    // buffer-backed (the compositor makes glyph masks this way): bytesPerRow 32 for a 16-wide texture
    { id<MTLBuffer> bb = [dev newBufferWithLength:32 * 16 options:MTLResourceStorageModeShared]; memset(bb.contents, 0xCC, 32 * 16);
      for (int y = 0; y < 16; y++) memcpy((uint8_t *)bb.contents + y * 32, pat + y * 16, 16);
      MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatA8Unorm width:16 height:16 mipmapped:NO]; td.usage = MTLTextureUsageShaderRead; td.storageMode = MTLStorageModeShared;
      id<MTLTexture> tb = [bb newTextureWithDescriptor:td offset:0 bytesPerRow:32];
      AG_CHECK("A8 texture over a buffer (bytesPerRow 32) non-nil", tb != nil, "");
      if (tb) { g2_cmp("buffer-backed sampled (0,0,0,a)", g2_sample(dev, q, lib, "g2_tex2d", tb, 0, NO, 0), exp, 0);
                uint8_t g2b[256]; memset(g2b, 0, 256); [tb getBytes:g2b bytesPerRow:16 fromRegion:MTLRegionMake2D(0, 0, 16, 16) mipmapLevel:0];
                AG_CHECK("buffer-backed getBytes", !memcmp(g2b, pat, 256), "raw bytes"); } }
    // render target use of A8: informational (the bundle cannot remap a fragment's alpha onto an R8 attachment, so it refuses A8 render targets)
    { MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatA8Unorm width:16 height:16 mipmapped:NO]; td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead; td.storageMode = MTLStorageModePrivate;
      id<MTLTexture> rt = [dev newTextureWithDescriptor:td];
      AG_INFO("A8 render target (usage RenderTarget|ShaderRead)", "%s", rt ? "created" : "nil");
      if (rt) { BOOL ok = g2_flat(dev, q, lib, rt, 0, YES, MTLClearColorMake(0.1, 0.2, 0.3, 0.8), NULL, 0, 0, 0, 0, 16, 16); NSData *d = ok ? n_readback(dev, q, rt, 16, 16, 1) : nil;
                AG_INFO("A8 clear (0.1,0.2,0.3,0.8)", "first byte %d (204 = alpha*255, 26 = red*255)", d ? ((const uint8_t *)d.bytes)[0] : -1); } }
}

// ---- g3: 3D textures ----
static MTLTextureDescriptor *g3_desc(MTLPixelFormat pf, int w, int h, int d, int mips, MTLTextureUsage u, MTLStorageMode sm) {
    MTLTextureDescriptor *td = [MTLTextureDescriptor new]; td.textureType = MTLTextureType3D; td.pixelFormat = pf; td.width = w; td.height = h; td.depth = d; td.mipmapLevelCount = mips; td.usage = u; td.storageMode = sm; return td;
}
static void g3_texel(uint8_t *p, int x, int y, int k) { p[0] = (uint8_t)(x * 16 + k); p[1] = (uint8_t)(y * 16 + k * 2); p[2] = (uint8_t)(k * 50 + 5); p[3] = (uint8_t)(255 - k); }   // RGBA bytes
static void g3_run(id<MTLDevice> dev, id<MTLCommandQueue> q, id<MTLLibrary> lib) {
    printf("mtlprobe: gaps2 g3 ---- 3D textures\n");
    enum { TW = 16, TH = 16, TD = 4 };
    static uint8_t vol[TD][TH][TW][4], model[TD][TH][TW][4], back[TD][TH][TW][4];
    for (int k = 0; k < TD; k++) for (int y = 0; y < TH; y++) for (int x = 0; x < TW; x++) g3_texel(vol[k][y][x], x, y, k);
    memcpy(model, vol, sizeof vol);
    id<MTLTexture> t = [dev newTextureWithDescriptor:g3_desc(MTLPixelFormatRGBA8Unorm, TW, TH, TD, 1, MTLTextureUsageShaderRead, N_MANAGED)];
    AG_CHECK("RGBA8 16x16x4 3D create", t != nil, "");
    if (t) {
        AG_CHECK("properties", t.textureType == MTLTextureType3D && t.width == TW && t.height == TH && t.depth == TD && t.arrayLength == 1 && t.mipmapLevelCount == 1 && t.pixelFormat == MTLPixelFormatRGBA8Unorm,
                 "type %lu %lux%lux%lu mips %lu array %lu", (unsigned long)t.textureType, (unsigned long)t.width, (unsigned long)t.height, (unsigned long)t.depth, (unsigned long)t.mipmapLevelCount, (unsigned long)t.arrayLength);
        [t replaceRegion:MTLRegionMake3D(0, 0, 0, TW, TH, TD) mipmapLevel:0 slice:0 withBytes:vol bytesPerRow:TW * 4 bytesPerImage:TW * TH * 4];
        memset(back, 0, sizeof back); [t getBytes:back bytesPerRow:TW * 4 bytesPerImage:TW * TH * 4 fromRegion:MTLRegionMake3D(0, 0, 0, TW, TH, TD) mipmapLevel:0 slice:0];
        AG_CHECK("replaceRegion/getBytes 16x16x4 round trip", !memcmp(vol, back, sizeof vol), "bytes");
        // partial box (2,3,1) 5x4x2 with padded source rows (bytesPerRow 32, bytesPerImage 128)
        uint8_t box[2][4][8][4]; memset(box, 0x5A, sizeof box);
        for (int k = 0; k < 2; k++) for (int y = 0; y < 4; y++) for (int x = 0; x < 5; x++) { box[k][y][x][0] = (uint8_t)(100 + x); box[k][y][x][1] = (uint8_t)(110 + y); box[k][y][x][2] = (uint8_t)(120 + k); box[k][y][x][3] = 77;
                                                                                              memcpy(model[1 + k][3 + y][2 + x], box[k][y][x], 4); }
        [t replaceRegion:MTLRegionMake3D(2, 3, 1, 5, 4, 2) mipmapLevel:0 slice:0 withBytes:box bytesPerRow:32 bytesPerImage:128];
        memset(back, 0, sizeof back); [t getBytes:back bytesPerRow:TW * 4 bytesPerImage:TW * TH * 4 fromRegion:MTLRegionMake3D(0, 0, 0, TW, TH, TD) mipmapLevel:0 slice:0];
        AG_CHECK("partial 3D replaceRegion (2,3,1) 5x4x2, padded source", !memcmp(model, back, sizeof model), "rest untouched");
        uint8_t sub[2][4][8][4]; memset(sub, 0, sizeof sub);
        [t getBytes:sub bytesPerRow:32 bytesPerImage:128 fromRegion:MTLRegionMake3D(2, 3, 1, 5, 4, 2) mipmapLevel:0 slice:0];
        int bs = 0; for (int k = 0; k < 2; k++) for (int y = 0; y < 4; y++) if (memcmp(sub[k][y], box[k][y], 5 * 4)) bs++;
        AG_CHECK("partial 3D getBytes with padded destination", bs == 0, "%d rows differ", bs);
        // restore the full volume for sampling
        [t replaceRegion:MTLRegionMake3D(0, 0, 0, TW, TH, TD) mipmapLevel:0 slice:0 withBytes:vol bytesPerRow:TW * 4 bytesPerImage:TW * TH * 4];
        // sample slice k (nearest): expected = texel(x,y,k) with the BGRA byte order of the target
        for (int k = 0; k < TD; k++) {
            uint8_t exp[256 * 4]; for (int y = 0; y < 16; y++) for (int x = 0; x < 16; x++) { uint8_t *e = exp + (y * 16 + x) * 4, *s = vol[k][y][x]; e[0] = s[2]; e[1] = s[1]; e[2] = s[0]; e[3] = s[3]; }
            char tag[64]; snprintf(tag, sizeof tag, "3D sample z=%d.5/4 nearest", k);
            g2_cmp(tag, g2_sample(dev, q, lib, "g2_tex3d", t, (k + 0.5f) / TD, NO, 0), exp, 0);
        }
        { uint8_t exp[256 * 4]; for (int y = 0; y < 16; y++) for (int x = 0; x < 16; x++) { uint8_t *e = exp + (y * 16 + x) * 4; const uint8_t *a = vol[1][y][x], *b = vol[2][y][x];
              e[0] = (uint8_t)((a[2] + b[2] + 1) / 2); e[1] = (uint8_t)((a[1] + b[1] + 1) / 2); e[2] = (uint8_t)((a[0] + b[0] + 1) / 2); e[3] = (uint8_t)((a[3] + b[3] + 1) / 2); }
          g2_cmp("3D sample z=0.5 linear (halfway between slices 1 and 2), within 2 LSB", g2_sample(dev, q, lib, "g2_tex3d", t, 0.5f, YES, 0), exp, 2); }
        // blit a box out and in
        id<MTLBuffer> bo = [dev newBufferWithLength:128 * 2 options:MTLResourceStorageModeShared]; memset(bo.contents, 0x33, 256);
        id<MTLCommandBuffer> cb = [q commandBuffer]; id<MTLBlitCommandEncoder> be = [cb blitCommandEncoder];
        [be copyFromTexture:t sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(2, 3, 1) sourceSize:MTLSizeMake(5, 4, 2) toBuffer:bo destinationOffset:0 destinationBytesPerRow:32 destinationBytesPerImage:128];
        [be copyFromBuffer:bo sourceOffset:0 sourceBytesPerRow:32 sourceBytesPerImage:128 sourceSize:MTLSizeMake(5, 4, 2) toTexture:t destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(9, 8, 0)];
        [be endEncoding]; BOOL ok = n_run(cb, "3d blit");
        int bb = 0; const uint8_t (*bm)[4][8][4] = (const uint8_t (*)[4][8][4])bo.contents;
        for (int k = 0; k < 2; k++) for (int y = 0; y < 4; y++) if (memcmp(bm[k][y], vol[1 + k][3 + y][2], 5 * 4)) bb++;
        AG_CHECK("blit 3D box (2,3,1) 5x4x2 -> buffer (padded rows and images)", ok && bb == 0, "%d rows differ", bb);
        memcpy(model, vol, sizeof vol); for (int k = 0; k < 2; k++) for (int y = 0; y < 4; y++) for (int x = 0; x < 5; x++) memcpy(model[k][8 + y][9 + x], vol[1 + k][3 + y][2 + x], 4);
        memset(back, 0, sizeof back); [t getBytes:back bytesPerRow:TW * 4 bytesPerImage:TW * TH * 4 fromRegion:MTLRegionMake3D(0, 0, 0, TW, TH, TD) mipmapLevel:0 slice:0];
        AG_CHECK("blit buffer -> 3D box at (9,8,0)", !memcmp(model, back, sizeof model), "volume equals the CPU model");
    }
    // the compositor's descriptor: BGRA8 1x1x1 3D, Managed, ShaderRead (79 refusals in the first run)
    id<MTLTexture> t1 = [dev newTextureWithDescriptor:g3_desc(MTLPixelFormatBGRA8Unorm, 1, 1, 1, 1, MTLTextureUsageShaderRead, N_MANAGED)];
    AG_CHECK("BGRA8 1x1x1 3D create (the compositor's dummy)", t1 != nil, "");
    if (t1) { uint8_t px[4] = { 10, 20, 30, 255 }; [t1 replaceRegion:MTLRegionMake3D(0, 0, 0, 1, 1, 1) mipmapLevel:0 slice:0 withBytes:px bytesPerRow:4 bytesPerImage:4];
              uint8_t exp[256 * 4]; g2_solid(exp, px); g2_cmp("1x1x1 3D sampled (BGRA bytes 10,20,30,255)", g2_sample(dev, q, lib, "g2_tex3d", t1, 0.5f, NO, 0), exp, 0); }
    // Private 3D (upload by blit), non-power-of-two 5x3x2
    id<MTLTexture> tp = [dev newTextureWithDescriptor:g3_desc(MTLPixelFormatRGBA8Unorm, 16, 16, 3, 1, MTLTextureUsageShaderRead, MTLStorageModePrivate)];
    AG_CHECK("RGBA8 16x16x3 Private 3D create", tp != nil, "");
    if (tp) {
        id<MTLBuffer> up = [dev newBufferWithLength:16 * 16 * 4 * 3 options:MTLResourceStorageModeShared]; for (int k = 0; k < 3; k++) for (int y = 0; y < 16; y++) for (int x = 0; x < 16; x++) g3_texel((uint8_t *)up.contents + ((k * 16 + y) * 16 + x) * 4, x, y, k + 1);
        id<MTLCommandBuffer> cb = [q commandBuffer]; id<MTLBlitCommandEncoder> be = [cb blitCommandEncoder];
        [be copyFromBuffer:up sourceOffset:0 sourceBytesPerRow:64 sourceBytesPerImage:1024 sourceSize:MTLSizeMake(16, 16, 3) toTexture:tp destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
        [be endEncoding]; BOOL ok = n_run(cb, "3d private upload");
        uint8_t exp[256 * 4]; for (int y = 0; y < 16; y++) for (int x = 0; x < 16; x++) { uint8_t s[4]; g3_texel(s, x, y, 2); uint8_t *e = exp + (y * 16 + x) * 4; e[0] = s[2]; e[1] = s[1]; e[2] = s[0]; e[3] = s[3]; }
        if (ok) g2_cmp("Private 3D sampled at slice 1 of 3 (z=0.5)", g2_sample(dev, q, lib, "g2_tex3d", tp, 0.5f, NO, 0), exp, 0);
    }
}

// ---- g4: mipmapped 2D textures ----
static void g4_col(uint8_t *p, int L) { p[0] = (uint8_t)(L * 35 + 20); p[1] = (uint8_t)(255 - L * 30); p[2] = (uint8_t)(L * 17 + 3); p[3] = (uint8_t)(255 - L); }   // RGBA bytes of level L
static void g4_run(id<MTLDevice> dev, id<MTLCommandQueue> q, id<MTLLibrary> lib) {
    printf("mtlprobe: gaps2 g4 ---- mipmapped 2D textures\n");
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:64 height:64 mipmapped:YES]; td.usage = MTLTextureUsageShaderRead; td.storageMode = N_MANAGED;
    id<MTLTexture> t = [dev newTextureWithDescriptor:td];
    AG_CHECK("RGBA8 64x64 mipmapped create", t != nil, "");
    if (t) {
        AG_CHECK("mipmapLevelCount 7", t.mipmapLevelCount == 7, "%lu", (unsigned long)t.mipmapLevelCount);
        int badg = 0;
        for (int L = 0; L < 7; L++) {
            int lw = 64 >> L; uint8_t c[4]; g4_col(c, L); uint8_t *buf = malloc((size_t)lw * lw * 4), *rb = calloc((size_t)lw * lw, 4);
            for (int i = 0; i < lw * lw; i++) memcpy(buf + i * 4, c, 4);
            [t replaceRegion:MTLRegionMake2D(0, 0, lw, lw) mipmapLevel:L withBytes:buf bytesPerRow:(NSUInteger)lw * 4];
            [t getBytes:rb bytesPerRow:(NSUInteger)lw * 4 fromRegion:MTLRegionMake2D(0, 0, lw, lw) mipmapLevel:L];
            if (memcmp(buf, rb, (size_t)lw * lw * 4)) badg++; free(buf); free(rb);
        }
        AG_CHECK("replaceRegion/getBytes at every level 0..6 (a different colour per level)", badg == 0, "%d levels differ", badg);
        for (int L = 0; L < 7; L += 2) {
            uint8_t c[4], exp[256 * 4]; g4_col(c, L); uint8_t bgra[4] = { c[2], c[1], c[0], c[3] }; g2_solid(exp, bgra);
            char tag[64]; snprintf(tag, sizeof tag, "sample level(%d) nearest, mip filter nearest", L);
            g2_cmp(tag, g2_sample(dev, q, lib, "g2_lod", t, (float)L, NO, 1), exp, 0);
        }
        { uint8_t a[4], b[4], exp[256 * 4], bgra[4]; g4_col(a, 1); g4_col(b, 2); for (int k = 0; k < 4; k++) bgra[k == 0 ? 2 : k == 2 ? 0 : k] = (uint8_t)((a[k] + b[k] + 1) / 2); g2_solid(exp, bgra);
          g2_cmp("sample level(1.5), linear mip filter = mean of levels 1 and 2 (within 3 LSB)", g2_sample(dev, q, lib, "g2_lod", t, 1.5f, YES, 2), exp, 3); }
        // blit a level out
        id<MTLBuffer> bo = [dev newBufferWithLength:16 * 16 * 4 options:MTLResourceStorageModeShared]; memset(bo.contents, 0x44, 16 * 16 * 4);
        id<MTLCommandBuffer> cb = [q commandBuffer]; id<MTLBlitCommandEncoder> be = [cb blitCommandEncoder];
        [be copyFromTexture:t sourceSlice:0 sourceLevel:2 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(16, 16, 1) toBuffer:bo destinationOffset:0 destinationBytesPerRow:64 destinationBytesPerImage:1024];
        [be endEncoding]; BOOL ok = n_run(cb, "g4 blit level 2"); uint8_t c2[4]; g4_col(c2, 2); int bd = 0; for (int i = 0; i < 256; i++) if (memcmp((uint8_t *)bo.contents + i * 4, c2, 4)) bd++;
        AG_CHECK("blit level 2 (16x16) -> buffer", ok && bd == 0, "%d of 256 texels differ", bd);
    }
    // non-power-of-two: 100x60, 7 levels (sizes 100x60, 50x30, 25x15, 12x7, 6x3, 3x1, 1x1)
    { MTLTextureDescriptor *nd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:100 height:60 mipmapped:YES]; nd.usage = MTLTextureUsageShaderRead; nd.storageMode = N_MANAGED;
      id<MTLTexture> n = [dev newTextureWithDescriptor:nd]; AG_CHECK("RGBA8 100x60 mipmapped create", n != nil && n.mipmapLevelCount == 7, "levels %lu", n ? (unsigned long)n.mipmapLevelCount : 0);
      if (n) { static const int lw[7] = { 100, 50, 25, 12, 6, 3, 1 }, lh[7] = { 60, 30, 15, 7, 3, 1, 1 }; int bad = 0;
               for (int L = 0; L < 7; L++) { uint8_t *s = malloc((size_t)lw[L] * lh[L] * 4), *r = calloc((size_t)lw[L] * lh[L], 4); for (int i = 0; i < lw[L] * lh[L] * 4; i++) s[i] = (uint8_t)(i * 7 + L * 31);
                   [n replaceRegion:MTLRegionMake2D(0, 0, lw[L], lh[L]) mipmapLevel:L withBytes:s bytesPerRow:(NSUInteger)lw[L] * 4]; [n getBytes:r bytesPerRow:(NSUInteger)lw[L] * 4 fromRegion:MTLRegionMake2D(0, 0, lw[L], lh[L]) mipmapLevel:L];
                   if (memcmp(s, r, (size_t)lw[L] * lh[L] * 4)) bad++; free(s); free(r); }
               AG_CHECK("non-power-of-two level sizes round trip (100x60 .. 1x1)", bad == 0, "%d levels differ", bad); } }
    // generateMipmapsForTexture: level 0 pattern, the box-filtered chain compared with the CPU (each level against the previous level READ BACK, so errors do not accumulate)
    { MTLTextureDescriptor *gd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:64 height:64 mipmapped:YES]; gd.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget; gd.storageMode = N_MANAGED;
      id<MTLTexture> g = [dev newTextureWithDescriptor:gd]; AG_CHECK("RGBA8 64x64 mipmapped (ShaderRead|RenderTarget) create", g != nil, "");
      if (g) { uint8_t *l0 = malloc(64 * 64 * 4); for (int y = 0; y < 64; y++) for (int x = 0; x < 64; x++) { uint8_t *p = l0 + (y * 64 + x) * 4; p[0] = (uint8_t)(x * 4); p[1] = (uint8_t)(y * 4); p[2] = (uint8_t)((x ^ y) * 4); p[3] = (uint8_t)(200 + (x & 7)); }
               [g replaceRegion:MTLRegionMake2D(0, 0, 64, 64) mipmapLevel:0 withBytes:l0 bytesPerRow:256];
               id<MTLCommandBuffer> cb = [q commandBuffer]; id<MTLBlitCommandEncoder> be = [cb blitCommandEncoder]; [be generateMipmapsForTexture:g]; [be endEncoding]; BOOL ok = n_run(cb, "generateMipmaps");
               int worst = 0, badl = 0; uint8_t *prev = l0; int pw = 64;
               for (int L = 1; L < 7 && ok; L++) { int lw = 64 >> L; uint8_t *cur = calloc((size_t)lw * lw, 4); [g getBytes:cur bytesPerRow:(NSUInteger)lw * 4 fromRegion:MTLRegionMake2D(0, 0, lw, lw) mipmapLevel:L];
                   int mx = 0; for (int y = 0; y < lw; y++) for (int x = 0; x < lw; x++) for (int c = 0; c < 4; c++) { int s = prev[((2*y)*pw + 2*x)*4+c] + prev[((2*y)*pw + 2*x+1)*4+c] + prev[((2*y+1)*pw + 2*x)*4+c] + prev[((2*y+1)*pw + 2*x+1)*4+c];
                       int e = (s + 2) / 4, d = abs(e - cur[(y*lw + x)*4+c]); if (d > mx) mx = d; }
                   if (mx > 1) badl++; if (mx > worst) worst = mx; if (prev != l0) free(prev); prev = cur; pw = lw; }
               if (prev != l0) free(prev); free(l0);
               AG_CHECK("generateMipmapsForTexture: levels 1..6 equal the 2x2 box of the previous level (within 1 LSB)", ok && badl == 0, "%d levels off, worst difference %d", badl, worst); } }
    // render into a mip level: level 0 filled with colour A by the CPU, level 1 rendered (clear B + flat quad C over (4,4)-(12,12) of 16x16), then sampled with level(1) and read back
    { MTLTextureDescriptor *rd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:32 height:32 mipmapped:YES]; rd.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget; rd.storageMode = N_MANAGED;
      id<MTLTexture> r = [dev newTextureWithDescriptor:rd]; AG_CHECK("BGRA8 32x32 mipmapped RenderTarget create", r != nil && r.mipmapLevelCount == 6, "levels %lu", r ? (unsigned long)r.mipmapLevelCount : 0);
      if (r) { uint8_t *a = malloc(32 * 32 * 4); for (int i = 0; i < 32 * 32; i++) { a[i*4] = 11; a[i*4+1] = 22; a[i*4+2] = 33; a[i*4+3] = 255; }
               [r replaceRegion:MTLRegionMake2D(0, 0, 32, 32) mipmapLevel:0 withBytes:a bytesPerRow:128];
               float C[4] = { 200.0f / 255, 100.0f / 255, 50.0f / 255, 1.0f };   // RGBA -> BGRA bytes 50,100,200,255
               BOOL ok = g2_flat(dev, q, lib, r, 1, YES, MTLClearColorMake(1.0, 0.0, 0.0, 1.0), C, 4, 4, 12, 12, 16, 16);
               uint8_t exp[256 * 4]; for (int y = 0; y < 16; y++) for (int x = 0; x < 16; x++) { uint8_t *e = exp + (y * 16 + x) * 4; BOOL in = x >= 4 && x < 12 && y >= 4 && y < 12;
                   if (in) { e[0] = 50; e[1] = 100; e[2] = 200; e[3] = 255; } else { e[0] = 0; e[1] = 0; e[2] = 255; e[3] = 255; } }
               uint8_t got1[256 * 4] = {0}; [r getBytes:got1 bytesPerRow:64 fromRegion:MTLRegionMake2D(0, 0, 16, 16) mipmapLevel:1];
               AG_CHECK("render pass into level 1: getBytes(level 1) = clear + quad", ok && !memcmp(got1, exp, sizeof exp), "BGRA bytes");
               uint8_t *g0 = calloc(32 * 32, 4); [r getBytes:g0 bytesPerRow:128 fromRegion:MTLRegionMake2D(0, 0, 32, 32) mipmapLevel:0];
               AG_CHECK("level 0 untouched by the level-1 pass", !memcmp(g0, a, 32 * 32 * 4), "bytes"); free(g0); free(a);
               g2_cmp("sampled with level(1) = the rendered level", g2_sample(dev, q, lib, "g2_lod", r, 1.0f, NO, 1), exp, 0); } }
    // the compositor's descriptor: BGRA8 192x128, 4 levels, Private, RenderTarget|ShaderRead; clear level 0, generate, sample level 3 (24x16)
    { MTLTextureDescriptor *cd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:192 height:128 mipmapped:YES]; cd.mipmapLevelCount = 4; cd.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget; cd.storageMode = MTLStorageModePrivate;
      id<MTLTexture> c = [dev newTextureWithDescriptor:cd]; AG_CHECK("BGRA8 192x128 4 levels Private (the compositor's) create", c != nil && c.mipmapLevelCount == 4, "levels %lu", c ? (unsigned long)c.mipmapLevelCount : 0);
      if (c) { BOOL ok = g2_flat(dev, q, lib, c, 0, YES, MTLClearColorMake(40.0 / 255, 80.0 / 255, 160.0 / 255, 1.0), NULL, 0, 0, 0, 0, 192, 128);
               id<MTLCommandBuffer> cb = [q commandBuffer]; id<MTLBlitCommandEncoder> be = [cb blitCommandEncoder]; [be generateMipmapsForTexture:c]; [be endEncoding]; ok = ok && n_run(cb, "generateMipmaps private");
               uint8_t bgra[4] = { 160, 80, 40, 255 }, exp[256 * 4]; g2_solid(exp, bgra);
               if (ok) g2_cmp("generated level 3 (24x16) of a cleared texture is the clear colour (within 1 LSB)", g2_sample(dev, q, lib, "g2_lod", c, 3.0f, NO, 1), exp, 1); } }
}

// ---- refuse (bundle behaviour: nil + log; real Metal asserts on some of these, so the host Mac does not run this) ----
#if !defined(__arm64__)
static void g2_refuse(id<MTLDevice> dev) {
    printf("mtlprobe: gaps2 refuse ---- descriptors the bundle must refuse (nil)\n");
    { MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatA8Unorm width:16 height:16 mipmapped:NO]; d.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead; d.storageMode = MTLStorageModePrivate;
      AG_CHECK("A8Unorm RenderTarget -> nil", [dev newTextureWithDescriptor:d] == nil, "no attachment swizzle exists in Vulkan"); }
    { MTLTextureDescriptor *d = g3_desc(MTLPixelFormatRGBA8Unorm, 8, 8, 8, 1, MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead, MTLStorageModePrivate);
      AG_CHECK("3D RenderTarget -> nil", [dev newTextureWithDescriptor:d] == nil, "3D slices are not render targets here"); }
    { MTLTextureDescriptor *d = g3_desc(MTLPixelFormatRGBA8Unorm, 8, 8, 8, 1, MTLTextureUsageShaderRead, MTLStorageModePrivate); d.arrayLength = 2;
      AG_CHECK("3D with arrayLength 2 -> nil", [dev newTextureWithDescriptor:d] == nil, ""); }
    { MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:64 height:64 mipmapped:NO]; d.mipmapLevelCount = 20; d.usage = MTLTextureUsageShaderRead; d.storageMode = MTLStorageModePrivate;
      AG_CHECK("64x64 with 20 mip levels -> nil", [dev newTextureWithDescriptor:d] == nil, "more than log2(64)+1 = 7"); }
    { MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:64 height:64 mipmapped:NO]; d.depth = 3; d.usage = MTLTextureUsageShaderRead; d.storageMode = MTLStorageModePrivate;
      AG_CHECK("2D with depth 3 -> nil", [dev newTextureWithDescriptor:d] == nil, ""); }
    { MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:64 height:64 mipmapped:NO]; d.textureType = MTLTextureType2DMultisample; d.sampleCount = 4; d.usage = MTLTextureUsageShaderRead; d.storageMode = MTLStorageModePrivate;
      AG_CHECK("multisample 2D -> nil", [dev newTextureWithDescriptor:d] == nil, ""); }
    { IOSurfaceRef s = io_make2(64, 64, 4, 'BGRA'); id<MTLTexture> n = s ? io_tex2(dev, s, MTLPixelFormatA8Unorm, 64, 64, MTLTextureUsageShaderRead) : nil;
      AG_CHECK("A8Unorm descriptor on a 4 byte/element IOSurface -> nil", s && n == nil, "bytes per element mismatch"); }
    { IOSurfaceRef s = io_make2(64, 64, 4, 'BGRA'); id<MTLTexture> n = s ? io_tex2(dev, s, MTLPixelFormatRGBA16Float, 64, 64, MTLTextureUsageShaderRead) : nil;
      AG_CHECK("RGBA16Float descriptor on a 4 byte/element IOSurface -> nil", s && n == nil, "bytes per element mismatch"); }
}
#endif

static int cmd_gaps2(const char *only, uint64_t rid, int haveRid) {
    id<MTLDevice> dev = pick_device(rid, haveRid); if (!dev) return 2;
    ag_fails = 0;
    #define GRUN(nm) (only && !strcmp(only, nm))
    BOOL all = !only || !strcmp(only, "all");
#if !defined(__arm64__)
    if (GRUN("refuse") || all) g2_refuse(dev);
    if (GRUN("refuse")) { printf(ag_fails ? "mtlprobe: FAIL gaps2 (%d)\n" : "mtlprobe: PASS gaps2 (refuse)\n", ag_fails); return ag_fails ? 1 : 0; }
#endif
    id<MTLLibrary> lib = g2_lib(dev); if (!lib) return 2;
    id<MTLCommandQueue> q = [dev newCommandQueue];
    if (GRUN("g1") || all) g1_run(dev, q, lib);
    if (GRUN("g2") || all) g2_a8(dev, q, lib);
    if (GRUN("g3") || all) g3_run(dev, q, lib);
    if (GRUN("g4") || all) g4_run(dev, q, lib);
    printf(ag_fails ? "mtlprobe: FAIL gaps2 (%d)\n" : "mtlprobe: PASS gaps2\n", ag_fails);
    return ag_fails ? 1 : 0;
}

// initsel (#11 H2): the device and queue selectors that SkyLight's CompositorMetal::new_compositor and MPSCore's MPSDevice ctors send at compositor
// init (carved from the 26.6.2 x86_64 cache), each: which class implements it. No calls (inherited implementations can SEGV): use `devcall2` per selector.
static void is_who(const char *tag, id obj, const char *sel) {
    SEL s = sel_registerName(sel); Class c = object_getClass(obj), owner = NULL;
    for (Class k = c; k; k = class_getSuperclass(k)) { unsigned n; Method *ms = class_copyMethodList(k, &n); for (unsigned i = 0; i < n; i++) if (method_getName(ms[i]) == s) { owner = k; break; } free(ms); if (owner) break; }
    printf("INITSEL\t%s\t%s\t%s\t%s\n", tag, sel, owner ? class_getName(owner) : "MISSING", [obj respondsToSelector:s] ? "responds" : "NO");
}
static int cmd_initsel(uint64_t rid, int haveRid, int nextra, char **extra) {
    id<MTLDevice> dev = pick_device(rid, haveRid); if (!dev) return 2;
    if (nextra) { for (int i = 0; i < nextra; i++) is_who("device", dev, extra[i]); return 0; }   // initsel <selector>...: just those, no queue
    static const char *dsels[] = { "registryID", "isRemovable", "supportPriorityBand", "peerGroupID", "recommendedMaxWorkingSetSize", "compilerPropagatesThreadPriority:", "maxTextureWidth2D", "maxTextureHeight2D",
        "newSharedEvent", "newEvent", "acceleratorPort", "isHeadless", "newLibraryWithURL:error:", "newDepthStencilStateWithDescriptor:", "newCommandQueue",
        "targetDeviceArchitecture", "maxComputeThreadgroupMemory", "maxTextureLayers", "minimumLinearTextureAlignmentForPixelFormat:", "isFloat32FilteringSupported", "supportsFamily:",
        "supports32BitFloatFiltering", "supportsArrayOfTextures", "supportsAtomicFloat", "supportsAtomicWaitNotify", "supportsCommandBufferJump", "supportsComputeCompressedTextureWrite", "supportsConditionalLoadStore",
        "supportsFP8", "supportsFloat16BCubicFiltering", "supportsMXU", "supportsNorm16BCubicFiltering", "supportsQuadShufflesAndBroadcast", "supportsReadWriteTextureArgumentsTier2", "supportsSIMDReduction",
        "supportsSIMDShuffleAndFill", "supportsSIMDShufflesAndBroadcast", "supportsTextureWriteRoundingMode:", "supportsWritableArrayOfTextures", "newHeapWithDescriptor:", "heapBufferSizeAndAlignWithLength:options:",
        "heapTextureSizeAndAlignWithDescriptor:", "supportsNativeHardwareFP16", "supportsDynamicLibraries", "supportsFunctionPointers", "supportsInt64", "supportsPullModelInterpolation", "supportsShaderBarycentricCoordinates",
        "supportsMeshShaders", "supportsPrimitiveMotionBlur", "supportsRayTracingMultiLevelInstancing", "supportsRayTracingTraversalMetrics", "supportsVertexAmplification", "supportsVertexAmplificationCount:", "supportsSharedStorageHeapResources",
        "supportsGlobalVariableBindingInDylibs", "supportsGlobalVariableRelocationCompute", "supportsGlobalVariableRelocationRender", "maxAccelerationStructureTraversalDepth", "requiresRaytracingEmulation", "indirectArgumentBufferCapabilities",
        "deviceSupportsFeatureSet:", "minLinearTexturePitchAlignmentForDescriptor:mustMatchExactly:", "supportsCounterSampling:", "argumentBuffersSupport", "readWriteTextureSupport", "newIOCommandQueueWithDescriptor:error:",
        "currentAllocatedSize", "maxBufferLength", "maxThreadgroupMemoryLength", "hasUnifiedMemory", "isLowPower", "isBuiltIn", "isSlotted", "getBuiltInGPUProperties:transferRate:", "initialKernelCommandShmemSize", "initialSegmentListShmemSize", NULL };
    for (int i = 0; dsels[i]; i++) is_who("device", dev, dsels[i]);
    id<MTLCommandQueue> q = [dev newCommandQueue];
    static const char *qsels[] = { "setCompletionQueue:", "setSubmissionQueue:", "setGPUPriority:", "_setGPUPriority:backgroundPriority:", "device", "setLabel:", "commandBuffer", "commandBufferWithDescriptor:", "commandBufferWithUnretainedReferences", NULL };
    for (int i = 0; qsels[i]; i++) is_who("queue", q, qsels[i]);
    return 0;
}
// devcall2 <b|q|o|i> <selector> [uint-arg]: like devcall with the right return width (BOOL, 64-bit, object, int); one call per process.
static int cmd_devcall2(const char *kind, const char *sel, int hasArg, unsigned long arg, uint64_t rid, int haveRid) {
    id<MTLDevice> dev = pick_device(rid, haveRid); if (!dev) return 2;
    SEL s = sel_registerName(sel);
    if (![(id)dev respondsToSelector:s]) { printf("devcall2 %s: device does NOT respond\n", sel); return 1; }
    printf("devcall2 %s: calling\n", sel); fflush(stdout);
    if (kind[0] == 'b') { BOOL v = hasArg ? ((BOOL (*)(id, SEL, unsigned long))objc_msgSend)(dev, s, arg) : ((BOOL (*)(id, SEL))objc_msgSend)(dev, s); printf("devcall2 %s = %d\n", sel, (int)v); }
    else if (kind[0] == 'q') { unsigned long long v = hasArg ? ((unsigned long long (*)(id, SEL, unsigned long))objc_msgSend)(dev, s, arg) : ((unsigned long long (*)(id, SEL))objc_msgSend)(dev, s); printf("devcall2 %s = %llu (0x%llx)\n", sel, v, v); }
    else if (kind[0] == 'i') { int v = hasArg ? ((int (*)(id, SEL, unsigned long))objc_msgSend)(dev, s, arg) : ((int (*)(id, SEL))objc_msgSend)(dev, s); printf("devcall2 %s = %d\n", sel, v); }
    else { id o = hasArg ? ((id (*)(id, SEL, unsigned long))objc_msgSend)(dev, s, arg) : ((id (*)(id, SEL))objc_msgSend)(dev, s);
        printf("devcall2 %s = %p class %s\n", sel, (__bridge void *)o, o ? class_getName(object_getClass(o)) : "-");
        if (o && !strcmp(sel, "targetDeviceArchitecture")) {
            for (const char *k2 = "cpuType"; k2; k2 = (k2[0] == 'c') ? "subType" : NULL) { SEL s2 = sel_registerName(k2); if ([o respondsToSelector:s2]) { int v = ((int (*)(id, SEL))objc_msgSend)(o, s2); printf("devcall2 targetDeviceArchitecture.%s = %d (0x%x)\n", k2, v, v); } else printf("devcall2 targetDeviceArchitecture.%s: not implemented\n", k2); }
            printf("description: %s\n", [[o description] UTF8String]); } }
    return 0;
}

// heap (#11 H3): MTLHeap through the compositor's path (-newHeapWithDescriptor:). Every expectation was measured on an Apple-silicon Mac first. Placement heaps, mismatched
// options and size 0 are bundle-specific (an Apple-silicon Mac aborts in its validation layer on mismatches; it supports placement), so those run on x86_64 only.
#define HP_CHECK(tag, cond, ...) do { int ok_ = (cond) ? 1 : 0; printf("mtlprobe: heap %s: %s: ", ok_ ? "ok  " : "FAIL", tag); printf(__VA_ARGS__); printf("\n"); if (!ok_) ag_fails++; } while (0)
#define HP_MIB (1ULL << 20)
static id<MTLHeap> hp_make(id<MTLDevice> dev, NSUInteger size, MTLStorageMode sm, MTLHazardTrackingMode hz, MTLCPUCacheMode cc) {
    MTLHeapDescriptor *hd = [MTLHeapDescriptor new]; hd.size = size; hd.storageMode = sm; hd.hazardTrackingMode = hz; hd.cpuCacheMode = cc; return [dev newHeapWithDescriptor:hd];
}
static int cmd_heap(const char *only, uint64_t rid, int haveRid) {
    id<MTLDevice> dev = pick_device(rid, haveRid); if (!dev) return 2;
    ag_fails = 0;
    #define HRUN(nm) (!only || !strcmp(only, nm))
    // `heap props` and `heap refuse` (every refusal returns before any Vulkan object exists) create no buffer or texture, so they need neither the library nor a queue (nor the exclusive N48N client: they run
    // while WindowServer holds it); `alloc` and `use` need RADV.
    id<MTLLibrary> lib = nil; id<MTLCommandQueue> q = nil;
    if (!only || (strcmp(only, "props") && strcmp(only, "refuse"))) { lib = n_lib(dev); if (!lib) return 2; q = [dev newCommandQueue]; }
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:256 height:256 mipmapped:NO];
    td.storageMode = MTLStorageModePrivate; td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    MTLSizeAndAlign tsa = [dev heapTextureSizeAndAlignWithDescriptor:td];
    if (HRUN("props")) {
        printf("mtlprobe: heap ---- props\n");
        NSUInteger cur0 = dev.currentAllocatedSize;
        id<MTLHeap> h = hp_make(dev, 64 * HP_MIB, MTLStorageModePrivate, MTLHazardTrackingModeDefault, MTLCPUCacheModeDefaultCache);
        HP_CHECK("create", h != nil, "class %s", h ? class_getName([(id)h class]) : "-");
        if (h) {
            HP_CHECK("size/used/current", h.size == 64 * HP_MIB && h.usedSize == 0 && h.currentAllocatedSize == 64 * HP_MIB, "size %lu used %lu currentAllocatedSize %lu", (unsigned long)h.size, (unsigned long)h.usedSize, (unsigned long)h.currentAllocatedSize);
            HP_CHECK("modes", h.storageMode == MTLStorageModePrivate && h.cpuCacheMode == MTLCPUCacheModeDefaultCache && h.hazardTrackingMode == MTLHazardTrackingModeUntracked && h.type == MTLHeapTypeAutomatic,
                     "storage %lu cache %lu hazard %lu (Default becomes Untracked) type %lu", (unsigned long)h.storageMode, (unsigned long)h.cpuCacheMode, (unsigned long)h.hazardTrackingMode, (unsigned long)h.type);
            HP_CHECK("resourceOptions", h.resourceOptions == 0x120, "0x%lx", (unsigned long)h.resourceOptions);
            HP_CHECK("device", h.device == dev, "heap.device is the device");
            { NSString *ds = [(id)h description]; HP_CHECK("description", ds.length > 0, "%s", ds.UTF8String); }   // -[_MTLHeap description] -> -formattedDescription: must not recurse
            HP_CHECK("maxAvailable", [h maxAvailableSizeWithAlignment:256] == 64 * HP_MIB && [h maxAvailableSizeWithAlignment:16384] == 64 * HP_MIB, "empty heap: whole size at 256 and 16384");
            HP_CHECK("label", h.label == nil, "nil before set"); h.label = @"n48 heap"; HP_CHECK("label roundtrip", [h.label isEqualToString:@"n48 heap"], "%s", h.label.UTF8String);
            HP_CHECK("device currentAllocatedSize", dev.currentAllocatedSize == cur0 + 64 * HP_MIB, "delta %ld (heap size 67108864)", (long)dev.currentAllocatedSize - (long)cur0);
            NSUInteger a = [h setPurgeableState:MTLPurgeableStateKeepCurrent], b = [h setPurgeableState:MTLPurgeableStateNonVolatile], c = [h setPurgeableState:MTLPurgeableStateVolatile], d2 = [h setPurgeableState:MTLPurgeableStateKeepCurrent];
            HP_CHECK("setPurgeableState", a == 2 && b == 2 && c == 2 && d2 == 3, "Keep->%lu NonVolatile->%lu Volatile->%lu Keep->%lu (previous state; expect 2 2 2 3)", (unsigned long)a, (unsigned long)b, (unsigned long)c, (unsigned long)d2);
            [h setPurgeableState:MTLPurgeableStateNonVolatile];
        }
        id<MTLHeap> h2 = hp_make(dev, 1000, MTLStorageModeShared, MTLHazardTrackingModeTracked, MTLCPUCacheModeWriteCombined);
        HP_CHECK("small heap rounds to 16 KiB", h2 && h2.size == 16384 && h2.currentAllocatedSize == 16384 && h2.resourceOptions == 0x201 && h2.hazardTrackingMode == MTLHazardTrackingModeTracked && h2.cpuCacheMode == MTLCPUCacheModeWriteCombined,
                 "size %lu options 0x%lx (Shared | WriteCombined | Tracked = 0x201)", h2 ? (unsigned long)h2.size : 0, h2 ? (unsigned long)h2.resourceOptions : 0);
        id<MTLHeap> h3 = hp_make(dev, 3 * HP_MIB, MTLStorageModeShared, MTLHazardTrackingModeUntracked, MTLCPUCacheModeDefaultCache);
        HP_CHECK("shared untracked heap", h3 && h3.resourceOptions == 0x100 && h3.size == 3 * HP_MIB, "options 0x%lx", h3 ? (unsigned long)h3.resourceOptions : 0);
        NSUInteger cur1 = dev.currentAllocatedSize; h = nil; h2 = nil; h3 = nil;
        HP_CHECK("release returns the heap's bytes", dev.currentAllocatedSize == cur0, "currentAllocatedSize %lu -> %lu (start %lu)", (unsigned long)cur1, (unsigned long)dev.currentAllocatedSize, (unsigned long)cur0);
        MTLSizeAndAlign bsa = [dev heapBufferSizeAndAlignWithLength:1000 options:MTLResourceStorageModePrivate];
        HP_CHECK("heapBufferSizeAndAlign", bsa.size == 1000 && bsa.align >= 16 && !(bsa.align & (bsa.align - 1)), "size %lu align %lu", (unsigned long)bsa.size, (unsigned long)bsa.align);
        HP_CHECK("heapTextureSizeAndAlign", tsa.size >= 256 * 256 * 4 && tsa.align >= 16 && !(tsa.align & (tsa.align - 1)), "size %lu align %lu (device-specific; >= 262144 bytes)", (unsigned long)tsa.size, (unsigned long)tsa.align);
    }
    if (HRUN("alloc")) {
        printf("mtlprobe: heap ---- alloc\n");
        id<MTLHeap> h = hp_make(dev, 64 * HP_MIB, MTLStorageModePrivate, MTLHazardTrackingModeDefault, MTLCPUCacheModeDefaultCache);
        if (!h) { HP_CHECK("create", 0, "heap nil"); return 1; }
        NSUInteger cur0 = dev.currentAllocatedSize;
        id<MTLBuffer> b = [h newBufferWithLength:1000 options:MTLResourceStorageModePrivate];
        HP_CHECK("buffer", b && b.length == 1000 && b.heap == h && b.heapOffset == 0 && !b.isAliasable && b.resourceOptions == 0x120 && b.storageMode == MTLStorageModePrivate && b.hazardTrackingMode == MTLHazardTrackingModeUntracked,
                 "heap==h %d offset %lu aliasable %d options 0x%lx", b ? b.heap == h : 0, b ? (unsigned long)b.heapOffset : 0, b ? (int)b.isAliasable : 0, b ? (unsigned long)b.resourceOptions : 0);
        HP_CHECK("buffer allocatedSize", b && b.allocatedSize == 1000, "%lu", b ? (unsigned long)b.allocatedSize : 0);
        HP_CHECK("usedSize / maxAvailable", h.usedSize == 1000 && [h maxAvailableSizeWithAlignment:256] == 64 * HP_MIB - 1024 && [h maxAvailableSizeWithAlignment:16384] == 64 * HP_MIB - 16384, "used %lu max(256) %lu max(16384) %lu", (unsigned long)h.usedSize, (unsigned long)[h maxAvailableSizeWithAlignment:256], (unsigned long)[h maxAvailableSizeWithAlignment:16384]);
        HP_CHECK("device currentAllocatedSize unchanged by heap resources", dev.currentAllocatedSize == cur0, "delta %ld", (long)dev.currentAllocatedSize - (long)cur0);
        id<MTLTexture> t = [h newTextureWithDescriptor:td];
        HP_CHECK("texture", t && t.heap == h && t.heapOffset == 0 && t.allocatedSize == tsa.size && t.resourceOptions == 0x120, "heap==h %d offset %lu allocatedSize %lu (SA %lu) options 0x%lx", t ? t.heap == h : 0, t ? (unsigned long)t.heapOffset : 0, t ? (unsigned long)t.allocatedSize : 0, (unsigned long)tsa.size, t ? (unsigned long)t.resourceOptions : 0);
        HP_CHECK("used after texture", h.usedSize == 1000 + tsa.size, "used %lu", (unsigned long)h.usedSize);
        [t makeAliasable];
        HP_CHECK("makeAliasable", t.isAliasable && h.usedSize == 1000, "aliasable %d used %lu (returns the texture's bytes)", (int)t.isAliasable, (unsigned long)h.usedSize);
        t = nil; HP_CHECK("release after makeAliasable does not double-free", h.usedSize == 1000, "used %lu", (unsigned long)h.usedSize);
        b = nil; HP_CHECK("release returns the buffer", h.usedSize == 0 && [h maxAvailableSizeWithAlignment:256] == 64 * HP_MIB, "used %lu", (unsigned long)h.usedSize);
        id<MTLBuffer> big = [h newBufferWithLength:64 * HP_MIB + 1 options:MTLResourceStorageModePrivate]; HP_CHECK("larger than the heap", big == nil, "nil");
        big = [h newBufferWithLength:64 * HP_MIB options:MTLResourceStorageModePrivate]; HP_CHECK("exact fit", big && h.usedSize == 64 * HP_MIB && [h maxAvailableSizeWithAlignment:16] == 0, "used %lu", (unsigned long)h.usedSize);
        id<MTLBuffer> more = [h newBufferWithLength:16 options:MTLResourceStorageModePrivate]; HP_CHECK("full heap", more == nil, "nil");
        big = nil; HP_CHECK("reuse after free", h.usedSize == 0 && [h newBufferWithLength:4096 options:MTLResourceStorageModePrivate] != nil, "ok");
        // fragmentation: a(1000) c(1000), free a: the gap before c is 1024 bytes
        id<MTLBuffer> a = [h newBufferWithLength:1000 options:MTLResourceStorageModePrivate], c = [h newBufferWithLength:1000 options:MTLResourceStorageModePrivate];
        NSUInteger co = c.heapOffset; a = nil;
        HP_CHECK("fragmentation", co == 0 && h.usedSize == 1000 && [h maxAvailableSizeWithAlignment:256] == 64 * HP_MIB - 2048, "heapOffset %lu (an Apple-silicon Mac reports 0 for automatic heaps) used %lu max(256) %lu", (unsigned long)co, (unsigned long)h.usedSize, (unsigned long)[h maxAvailableSizeWithAlignment:256]);
        id<MTLBuffer> e = [h newBufferWithLength:64 * HP_MIB - 1000 options:MTLResourceStorageModePrivate]; HP_CHECK("no gap large enough", e == nil, "nil");
        e = [h newBufferWithLength:60 * HP_MIB options:MTLResourceStorageModePrivate]; HP_CHECK("fits after the used block", e != nil, "%s", e ? "ok" : "nil");
        // offset variants on an automatic heap
        id<MTLBuffer> ob = [h newBufferWithLength:100 options:MTLResourceStorageModePrivate offset:4096]; id<MTLTexture> ot = [h newTextureWithDescriptor:td offset:0];
        HP_CHECK("offset variants on an automatic heap", ob == nil && ot == nil, "both nil");
        // churn: 300 random alloc/free cycles leave nothing behind
        a = nil; c = nil; e = nil; srand(7); NSMutableArray *live = [NSMutableArray array]; int fails = 0;
        for (int i = 0; i < 300; i++) { if (live.count > 12 || (live.count && rand() % 3 == 0)) [live removeObjectAtIndex:rand() % live.count];
            id r = [h newBufferWithLength:(NSUInteger)(1 + rand() % 200000) options:MTLResourceStorageModePrivate]; if (r) [live addObject:r]; else fails++; }
        [live removeAllObjects];
        HP_CHECK("churn", h.usedSize == 0 && [h maxAvailableSizeWithAlignment:256] == 64 * HP_MIB, "used %lu max %lu (alloc failures %d)", (unsigned long)h.usedSize, (unsigned long)[h maxAvailableSizeWithAlignment:256], fails);
        // resources keep working after the heap reference is dropped
        id<MTLHeap> h4 = hp_make(dev, 2 * HP_MIB, MTLStorageModeShared, MTLHazardTrackingModeDefault, MTLCPUCacheModeDefaultCache);
        id<MTLBuffer> sb = [h4 newBufferWithLength:4096 options:MTLResourceStorageModeShared]; h4 = nil;
        HP_CHECK("heap outlives its handle", sb && sb.heap != nil && sb.heap.usedSize == 4096 && sb.contents != NULL, "heap size %lu used %lu", sb ? (unsigned long)sb.heap.size : 0, sb ? (unsigned long)sb.heap.usedSize : 0);
    }
    if (HRUN("use")) {
        printf("mtlprobe: heap ---- use\n");
        // (1) shared heap buffers: CPU write -> GPU copy to a normal buffer, and a Private-heap round trip
        id<MTLHeap> hs = hp_make(dev, 8 * HP_MIB, MTLStorageModeShared, MTLHazardTrackingModeDefault, MTLCPUCacheModeDefaultCache);
        id<MTLHeap> hv = hp_make(dev, 8 * HP_MIB, MTLStorageModePrivate, MTLHazardTrackingModeDefault, MTLCPUCacheModeDefaultCache);
        if (!hs || !hv) { HP_CHECK("heaps", 0, "nil"); return 1; }
        size_t n = 100000; id<MTLBuffer> sb = [hs newBufferWithLength:n options:MTLResourceStorageModeShared], pb = [hv newBufferWithLength:n options:MTLResourceStorageModePrivate];
        id<MTLBuffer> back = [dev newBufferWithLength:n options:MTLResourceStorageModeShared];
        HP_CHECK("buffers", sb && pb && back && sb.contents && !pb.contents, "shared heap buffer has contents, private has none");
        if (sb && pb && back) {
            for (size_t i = 0; i < n; i++) ((uint8_t *)sb.contents)[i] = (uint8_t)(i * 13 + 5); memset(back.contents, 0, n);
            id<MTLCommandBuffer> cb = [q commandBuffer]; id<MTLBlitCommandEncoder> be = [cb blitCommandEncoder];
            [be copyFromBuffer:sb sourceOffset:0 toBuffer:pb destinationOffset:0 size:n]; [be copyFromBuffer:pb sourceOffset:0 toBuffer:back destinationOffset:0 size:n]; [be endEncoding];
            BOOL ok = n_run(cb, "heap buffers"); HP_CHECK("shared -> private heap buffer -> shared round trip", ok && !memcmp(back.contents, sb.contents, n), "100000 bytes exact");
        }
        // (2) sample through a heap texture (Shared heap, BGRA8, replaceRegion) and render into a heap render target (Private heap)
        MTLTextureDescriptor *sd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:64 height:64 mipmapped:NO]; sd.storageMode = MTLStorageModeShared; sd.usage = MTLTextureUsageShaderRead;
        id<MTLTexture> st = [hs newTextureWithDescriptor:sd]; uint8_t pat[64 * 64 * 4]; n_pattern(pat);
        HP_CHECK("shared heap texture", st && st.heap == hs, "heap==hs %d", st ? st.heap == hs : 0);
        if (st) { [st replaceRegion:MTLRegionMake2D(0, 0, 64, 64) mipmapLevel:0 withBytes:pat bytesPerRow:256];
            NSData *g = ag_sample(dev, lib, q, st, "heap sample"); HP_CHECK("sample through a heap texture", g && !memcmp(g.bytes, pat, sizeof pat), "64x64 BGRA8 sampled exact (0 bytes differ expected: %d)", g ? (int)(memcmp(g.bytes, pat, sizeof pat) != 0) : -1); }
        id<MTLTexture> rt = [hv newTextureWithDescriptor:td];
        if (rt) {
            MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor]; rp.colorAttachments[0].texture = rt; rp.colorAttachments[0].loadAction = MTLLoadActionClear;
            rp.colorAttachments[0].clearColor = MTLClearColorMake(0.25, 0.5, 0.75, 1.0); rp.colorAttachments[0].storeAction = MTLStoreActionStore;
            id<MTLCommandBuffer> cb = [q commandBuffer]; id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:rp]; [re endEncoding];
            BOOL ok = n_run(cb, "heap clear"); NSData *g = ok ? n_readback(dev, q, rt, 256, 256, 4) : nil; int bad = 0;
            if (g) for (int i = 0; i < 256 * 256; i++) { const uint8_t *px = (const uint8_t *)g.bytes + i * 4; if (abs(px[0] - 191) > 1 || abs(px[1] - 128) > 1 || abs(px[2] - 64) > 1 || px[3] != 255) bad++; }
            HP_CHECK("render into a heap render target", g && bad == 0, "256x256 cleared to (0.25,0.5,0.75): %d of 65536 pixels differ (tolerance 1 LSB)", bad);
        } else HP_CHECK("heap render target", 0, "nil");
        // (3) useHeap: is accepted on all three encoders
        id<MTLCommandBuffer> cb = [q commandBuffer]; id<MTLBlitCommandEncoder> be = [cb blitCommandEncoder]; id<MTLComputeCommandEncoder> ce = nil; (void)ce;
        [be endEncoding]; id<MTLRenderCommandEncoder> re2 = nil; (void)re2;
        MTLRenderPassDescriptor *rp2 = [MTLRenderPassDescriptor renderPassDescriptor]; rp2.colorAttachments[0].texture = rt; rp2.colorAttachments[0].loadAction = MTLLoadActionLoad; rp2.colorAttachments[0].storeAction = MTLStoreActionStore;
        id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:rp2]; [re useHeap:hv]; [re endEncoding];
        id<MTLComputeCommandEncoder> ce2 = [cb computeCommandEncoder]; [ce2 useHeap:hv]; [ce2 endEncoding];
        HP_CHECK("useHeap on render and compute encoders", n_run(cb, "heap useHeap"), "command buffer completed");
    }
#if defined(__x86_64__)
    if (HRUN("refuse")) {
        printf("mtlprobe: heap ---- refuse (bundle-specific)\n");
        HP_CHECK("size 0", hp_make(dev, 0, MTLStorageModePrivate, MTLHazardTrackingModeDefault, MTLCPUCacheModeDefaultCache) == nil, "nil");
        MTLHeapDescriptor *pd = [MTLHeapDescriptor new]; pd.size = HP_MIB; pd.storageMode = MTLStorageModePrivate; pd.type = MTLHeapTypePlacement;
        HP_CHECK("placement heap", [dev newHeapWithDescriptor:pd] == nil, "nil (no aliasing exists here)");
        id<MTLHeap> h = hp_make(dev, HP_MIB, MTLStorageModePrivate, MTLHazardTrackingModeDefault, MTLCPUCacheModeDefaultCache);
        HP_CHECK("storage mode mismatch", h && [h newBufferWithLength:64 options:MTLResourceStorageModeShared] == nil && [h usedSize] == 0, "Shared buffer in a Private heap: nil, nothing charged");
        HP_CHECK("cache mode mismatch", [h newBufferWithLength:64 options:MTLResourceStorageModePrivate | MTLResourceCPUCacheModeWriteCombined] == nil && [h usedSize] == 0, "nil");
        HP_CHECK("tracked request in an untracked heap", [h newBufferWithLength:64 options:MTLResourceStorageModePrivate | MTLResourceHazardTrackingModeTracked] == nil, "nil");
        HP_CHECK("zero length", [h newBufferWithLength:0 options:MTLResourceStorageModePrivate] == nil, "nil");
        MTLTextureDescriptor *bad = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatA8Unorm width:16 height:16 mipmapped:NO]; bad.storageMode = MTLStorageModePrivate;
        HP_CHECK("unsupported pixel format", [h newTextureWithDescriptor:bad] == nil && h.usedSize == 0, "A8Unorm: nil, nothing charged");
        td.storageMode = MTLStorageModeShared; HP_CHECK("texture storage mismatch", [h newTextureWithDescriptor:td] == nil, "nil"); td.storageMode = MTLStorageModePrivate;
        id<MTLTexture> big = [h newTextureWithDescriptor:[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Float width:1024 height:1024 mipmapped:NO]]; (void)big;
        HP_CHECK("texture larger than the heap", big == nil, "16 MiB texture in a 1 MiB heap: nil");
    }
#endif
    printf("mtlprobe: heap: %d check(s) FAILED\n", ag_fails);
    printf(ag_fails == 0 ? "mtlprobe: PASS heap\n" : "mtlprobe: FAIL heap\n");
    return ag_fails ? 1 : 0;
}


// ---- #12 bundle robustness oracles: mtlprobe robust [r1|r2|touch|open] [--registry-id N] ----
// r1: fill_buf with N48M_FORCE_FALLBACK=1 (bundle, needs N48M_ALLOW) -> the pipeline is a VALID placeholder, the dispatches complete (status 4) and write
//     nothing (buffer stays 0xAA); WITHOUT the variable the same kernel must produce exact results (real Metal / bundle cache hit).
// r2: dispatch-time local size: fill_buf (translated at 64) with dispatchThreads and dispatchThreadgroups at tpt 1,16,32,64,128,256,1024 and write_tex
//     (translated at 8x8) at 8x8,16x4,4x16,32x2,2x32,16x16: every result must be exact.
// touch: one pipeline + one completed compute command buffer (the first completed cb is the bundle's clean marker). open: pipeline creation timing and a
//     second creation attempt after 2.5 s (R4 retry; with N48M_TEST_OPEN_REFUSE=n on the bundle).
static int rb_fill(id<MTLDevice> dev, id<MTLCommandQueue> q, id<MTLComputePipelineState> pa, NSUInteger tpt, BOOL threads, uint32_t n, uint32_t *bad, int *status, uint32_t *changed) {
    id<MTLBuffer> b = [dev newBufferWithLength:16384 options:MTLResourceStorageModeShared]; if (!b) return -1;
    memset(b.contents, 0xAA, 16384);
    id<MTLCommandBuffer> cb = [q commandBuffer]; id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
    [ce setComputePipelineState:pa]; [ce setBuffer:b offset:0 atIndex:0]; [ce setBytes:&n length:4 atIndex:1];
    if (threads) [ce dispatchThreads:MTLSizeMake(n, 1, 1) threadsPerThreadgroup:MTLSizeMake(tpt, 1, 1)];
    else [ce dispatchThreadgroups:MTLSizeMake((n + tpt - 1) / tpt, 1, 1) threadsPerThreadgroup:MTLSizeMake(tpt, 1, 1)];
    [ce endEncoding]; [cb commit]; [cb waitUntilCompleted];
    *status = (int)cb.status; if (cb.error) perr("rb_fill", cb.error);
    const uint32_t *o = b.contents; uint32_t bd = 0;
    for (uint32_t i = 0; i < 4096; i++) { uint32_t e = i < n ? (i * 2654435761u ^ (i >> 3)) : 0xAAAAAAAAu; if (o[i] != e) bd++; }
    uint32_t ch = 0; for (uint32_t i = 0; i < 4096; i++) if (o[i] != 0xAAAAAAAAu) ch++;
    *bad = bd; if (changed) *changed = ch; return 0;
}
static int cmd_robust(const char *sub, uint64_t rid, int haveRid) {
    id<MTLDevice> dev = pick_device(rid, haveRid); if (!dev) { printf("mtlprobe: robust: no device (declined?)\n"); return 2; }
    id<MTLLibrary> lib = n_lib(dev); if (!lib) return 2;
    NSError *err = nil; BOOL fb = getenv("N48M_FORCE_FALLBACK") && !strcmp(getenv("N48M_FORCE_FALLBACK"), "1");
    int fails = 0;
    #define RB(cond, ...) do { int ok_ = (cond) ? 1 : 0; printf("mtlprobe: robust %s: ", ok_ ? "ok  " : "FAIL"); printf(__VA_ARGS__); printf("\n"); if (!ok_) fails++; } while (0)
    if (!strcmp(sub, "open")) {
        double t0 = now_s();
        id<MTLComputePipelineState> p1 = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"fill_buf"] error:&err];
        printf("mtlprobe: robust open: first pipeline %s after %.2f s (%s)\n", p1 ? "non-nil" : "NIL", now_s() - t0, err ? err.localizedDescription.UTF8String : "no error");
        if (!p1) { usleep(2500000); err = nil; t0 = now_s();
            p1 = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"fill_buf"] error:&err];
            printf("mtlprobe: robust open: second pipeline (after 2.5 s) %s after %.2f s (%s)\n", p1 ? "non-nil" : "NIL", now_s() - t0, err ? err.localizedDescription.UTF8String : "no error"); }
        printf(p1 ? "mtlprobe: PASS robust open\n" : "mtlprobe: FAIL robust open\n"); return p1 ? 0 : 1;
    }
    id<MTLFunction> fa = [lib newFunctionWithName:@"fill_buf"], fw = [lib newFunctionWithName:@"write_tex"];
    id<MTLComputePipelineState> pa = [dev newComputePipelineStateWithFunction:fa error:&err]; perr("fill_buf", err);
    if (!pa && strcmp(sub, "r1pipe")) { printf("mtlprobe: FAIL robust (compute pipeline NIL: %s)\n", err ? err.localizedDescription.UTF8String : "?"); return 2; }
    if (!strcmp(sub, "r1pipe")) {   // pipeline creation only (no buffers/queue: works while another process holds N48N)
        id<MTLComputePipelineState> pw = [dev newComputePipelineStateWithFunction:fw error:&err];
        RB(pa != nil && pw != nil, "N48M_FORCE_FALLBACK=%d: fill_buf %s, write_tex %s (both must be non-nil placeholders when the fallback is forced)", fb, pa ? "non-nil" : "NIL", pw ? "non-nil" : "NIL");
        if (pa) RB([pa maxTotalThreadsPerThreadgroup] > 0 && [pa threadExecutionWidth] > 0, "placeholder answers maxTotalThreadsPerThreadgroup %lu threadExecutionWidth %lu", (unsigned long)[pa maxTotalThreadsPerThreadgroup], (unsigned long)[pa threadExecutionWidth]);
        printf(fails ? "mtlprobe: FAIL robust r1pipe\n" : "mtlprobe: PASS robust r1pipe\n"); return fails != 0;
    }
    id<MTLCommandQueue> q = [dev newCommandQueue];
    if (!strcmp(sub, "touch")) { uint32_t bad; int st; rb_fill(dev, q, pa, 64, YES, 1000, &bad, &st, NULL); RB(st == 4 && bad == 0, "touch: status %d, %u bad words", st, bad); printf(fails ? "mtlprobe: FAIL robust touch\n" : "mtlprobe: PASS robust touch\n"); return fails != 0; }
    if (!strcmp(sub, "r1")) {
        for (int th = 0; th < 2; th++) { uint32_t bad, ch; int st; rb_fill(dev, q, pa, 64, th, 1000, &bad, &st, &ch);
            if (fb) RB(st == 4 && ch == 0, "FORCE_FALLBACK %s: status %d (4 = Completed), %u words changed (expect 0: the dispatch is a no-op)", th ? "dispatchThreads" : "dispatchThreadgroups", st, ch);
            else RB(st == 4 && bad == 0, "real kernel %s: status %d, %u bad words", th ? "dispatchThreads" : "dispatchThreadgroups", st, bad); }
        // a second pipeline from another function; in fallback mode it is a placeholder too, and a following texture pass still works
        id<MTLComputePipelineState> pw = [dev newComputePipelineStateWithFunction:fw error:&err];
        RB(pw != nil, "write_tex pipeline non-nil (fallback %d)", fb);
        printf(fails ? "mtlprobe: FAIL robust r1\n" : "mtlprobe: PASS robust r1\n"); return fails != 0;
    }
    if (!strcmp(sub, "r2")) {
        NSUInteger tp[] = { 64, 1, 16, 32, 128, 256, 1024 };
        for (int k = 0; k < 7; k++) for (int th = 0; th < 2; th++) {
            uint32_t bad; int st; rb_fill(dev, q, pa, tp[k], th, 1000, &bad, &st, NULL);
            RB(st == 4 && bad == 0, "fill_buf tpt %lu %s n=1000: status %d, %u bad words", (unsigned long)tp[k], th ? "dispatchThreads" : "dispatchThreadgroups", st, bad); }
        id<MTLComputePipelineState> pw = [dev newComputePipelineStateWithFunction:fw error:&err]; perr("write_tex", err);
        if (!pw) { printf("mtlprobe: FAIL robust r2 (write_tex NIL)\n"); return 2; }
        NSUInteger tx[] = { 8, 16, 4, 32, 2, 16 }, ty[] = { 8, 4, 16, 2, 32, 16 };
        for (int k = 0; k < 6; k++) for (int th = 0; th < 2; th++) {
            int w = th ? 60 : 64;
            id<MTLTexture> t = n_tex(dev, MTLPixelFormatRGBA8Unorm, w, w, MTLTextureUsageShaderWrite | MTLTextureUsageShaderRead, MTLStorageModePrivate);
            id<MTLCommandBuffer> cb = [q commandBuffer]; id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
            [ce setComputePipelineState:pw]; [ce setTexture:t atIndex:0];
            if (th) [ce dispatchThreads:MTLSizeMake(w, w, 1) threadsPerThreadgroup:MTLSizeMake(tx[k], ty[k], 1)];
            else [ce dispatchThreadgroups:MTLSizeMake(w / tx[k], w / ty[k], 1) threadsPerThreadgroup:MTLSizeMake(tx[k], ty[k], 1)];
            [ce endEncoding]; [cb commit]; [cb waitUntilCompleted];
            int bad = 0; NSData *g = (cb.status == 4) ? n_readback(dev, q, t, w, w, 4) : nil;
            if (g) { const uint8_t *p = g.bytes; for (int y = 0; y < w; y++) for (int x = 0; x < w; x++) { uint8_t e[4] = { (uint8_t)x, (uint8_t)y, (uint8_t)((x * 3 + y) & 255), 255 }; if (memcmp(p + (y * w + x) * 4, e, 4)) bad++; } } else bad = -1;
            RB(cb.status == 4 && bad == 0, "write_tex tpt %lux%lu %s %dx%d: status %ld, %d bad pixels", (unsigned long)tx[k], (unsigned long)ty[k], th ? "dispatchThreads" : "dispatchThreadgroups", w, w, (long)cb.status, bad); }
        printf(fails ? "mtlprobe: FAIL robust r2\n" : "mtlprobe: PASS robust r2\n"); return fails != 0;
    }
    fprintf(stderr, "usage: mtlprobe robust r1|r2|touch|open\n"); return 64;
}

// ---- native #12: pipeline hot-swap oracle ----
// mtlprobe hotswap [--wait SECONDS] [--spv-src DIR] [--side-dir DIR] [--registry-id N]
// Needs the Navi48 device and the bundle's test hooks (root + N48M_ALLOW=1; the command sets the three N48M_TEST_* variables itself BEFORE the device is
// created): the bundle's spvcache is hidden, the side spvcache directory is a fresh private directory, and this process takes the WindowServer branch of
// the fallback rule. So the quad pipeline (quad_vs/quad_fs) is a spvcache MISS -> a FALLBACK (magenta) pipeline object. Steps:
//   1 draw with the fallback: expect magenta pixels and not the quad image;  2 encode (do not commit) a second command buffer with the fallback;
//   3 copy <sha>.meta.json then <sha>.spv of both shaders into the side dir (atomic rename, .spv last);  4 draw again with the SAME pipeline object every 100 ms
//   until the exact quad image appears (or --wait, default 3 s, runs out);  5 commit the command buffer of step 2 AFTER the swap: it must complete (status 4) and
//   still show the fallback (an in-flight command buffer keeps the fallback generation);  6 one more draw: exact quad image.
// On any other device (an Apple-silicon without the bundle) it refuses with a SKIP and exit 3.
static NSData *hs_sha_file_bytes(id fn) {
    SEL sel = NSSelectorFromString(@"bitcodeData");
    return (fn && [fn respondsToSelector:sel]) ? ((NSData *(*)(id, SEL))objc_msgSend)(fn, sel) : nil;
}
static NSString *hs_sha(NSData *d) {
    unsigned char h[CC_SHA256_DIGEST_LENGTH]; CC_SHA256(d.bytes, (CC_LONG)d.length, h);
    NSMutableString *m = [NSMutableString string]; for (int i = 0; i < CC_SHA256_DIGEST_LENGTH; i++) [m appendFormat:@"%02x", h[i]]; return m;
}
static BOOL hs_install(NSString *src, NSString *dst, NSString *sha, NSString *ext) {   // copy to a temp name in dst, then rename (atomic)
    NSData *d = [NSData dataWithContentsOfFile:[src stringByAppendingPathComponent:[sha stringByAppendingString:ext]]];
    if (!d.length) { printf("mtlprobe: FAIL hotswap: %s/%s%s missing or empty\n", src.UTF8String, sha.UTF8String, ext.UTF8String); return NO; }
    NSString *tmp = [dst stringByAppendingPathComponent:[NSString stringWithFormat:@".tmp-%@%@", sha, ext]], *fin = [dst stringByAppendingPathComponent:[sha stringByAppendingString:ext]];
    if (![d writeToFile:tmp atomically:NO] || rename(tmp.UTF8String, fin.UTF8String)) { printf("mtlprobe: FAIL hotswap: install %s\n", fin.UTF8String); return NO; }
    return YES;
}
static void hs_expected(uint8_t *exp) {   // the quad oracle's CPU reference (same as cmd_quad), BGRA
    uint8_t pat[64 * 64 * 4]; n_pattern(pat); float tint[4] = { 1.0f, 0.75f, 0.5f, 1.0f };
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        uint8_t *e = exp + (y * W + x) * 4;
        if (x >= 32 && x < 224 && y >= 32 && y < 224) {
            int tx = (int)floorf(((x + 0.5f - 32) / 192.0f) * 64.0f), ty = (int)floorf(((y + 0.5f - 32) / 192.0f) * 64.0f);
            const uint8_t *t = pat + (ty * 64 + tx) * 4;
            e[2] = (uint8_t)lrintf(t[0] / 255.0f * tint[0] * 255.0f); e[1] = (uint8_t)lrintf(t[1] / 255.0f * tint[1] * 255.0f);
            e[0] = (uint8_t)lrintf(t[2] / 255.0f * tint[2] * 255.0f); e[3] = (uint8_t)lrintf(t[3] / 255.0f * tint[3] * 255.0f);
        } else { e[0] = 30; e[1] = 20; e[2] = 10; e[3] = 255; }
    }
}
static id<MTLCommandBuffer> hs_encode(id<MTLCommandQueue> q, id<MTLRenderPipelineState> pso, id<MTLSamplerState> ss, id<MTLTexture> src, id<MTLTexture> tgt, id<MTLBuffer> vb, id<MTLBuffer> ib) {
    id<MTLCommandBuffer> cb = [q commandBuffer];
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = tgt; rp.colorAttachments[0].loadAction = MTLLoadActionClear; rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    rp.colorAttachments[0].clearColor = MTLClearColorMake(10.0 / 255, 20.0 / 255, 30.0 / 255, 1.0);
    id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:rp];
    float xf[4] = { 0.75f, 0.75f, 0, 0 }, tint[4] = { 1.0f, 0.75f, 0.5f, 1.0f };
    [re setRenderPipelineState:pso];
    [re setViewport:(MTLViewport){ 0, 0, W, H, 0, 1 }]; [re setScissorRect:(MTLScissorRect){ 0, 0, W, H }];
    [re setVertexBuffer:vb offset:0 atIndex:0]; [re setVertexBytes:xf length:sizeof xf atIndex:1]; [re setFragmentBytes:tint length:sizeof tint atIndex:0];
    [re setFragmentTexture:src atIndex:0]; [re setFragmentSamplerState:ss atIndex:0];
    [re drawIndexedPrimitives:MTLPrimitiveTypeTriangle indexCount:6 indexType:MTLIndexTypeUInt16 indexBuffer:ib indexBufferOffset:0];
    [re endEncoding];
    return cb;
}
static int hs_magenta(const uint8_t *bgra) { int n = 0; for (int i = 0; i < W * H; i++) if (bgra[i*4] == 255 && bgra[i*4+1] == 0 && bgra[i*4+2] == 255 && bgra[i*4+3] == 255) n++; return n; }
static int hs_diff(const uint8_t *got, const uint8_t *exp) { int n = 0; for (int i = 0; i < W * H; i++) if (memcmp(got + i * 4, exp + i * 4, 4)) n++; return n; }

static int cmd_hotswap(uint64_t rid, int haveRid, double waitS, const char *spvSrc, const char *sideDirArg) {
    char sidebuf[256]; const char *side = sideDirArg;
    if (!side) { snprintf(sidebuf, sizeof sidebuf, "/private/var/tmp/n48m-spv-probe.%d", (int)getpid()); side = sidebuf; }
    setenv("N48M_TEST_FALLBACK_AS_WS", "1", 1); setenv("N48M_TEST_HIDE_BUNDLE_SPV", "1", 1); setenv("N48M_TEST_SIDE_DIR", side, 1);   // read by the bundle at its first lookup (root + N48M_ALLOW=1 only)
    id<MTLDevice> dev = pick_device(rid, haveRid); if (!dev) return 2;
    if (![dev.name containsString:@"9070"]) { printf("mtlprobe: SKIP hotswap: %s is not the Navi48 device (the oracle needs the bundle with N48M_ALLOW=1 as root); nothing was run\n", dev.name.UTF8String); return 3; }
    if (geteuid() != 0 || !getenv("N48M_ALLOW")) { printf("mtlprobe: SKIP hotswap: needs root and N48M_ALLOW=1 (the bundle ignores the test hooks otherwise)\n"); return 3; }
    NSString *src = @(spvSrc ? spvSrc : getenv("N48M_PROBE_SPVSRC") ? getenv("N48M_PROBE_SPVSRC") : "/Library/GPUBundles/Navi48Metal.bundle/Contents/Resources/spvcache");
    id<MTLLibrary> lib = n_lib(dev); if (!lib) return 2;
    id<MTLFunction> vs = [lib newFunctionWithName:@"quad_vs"], fs = [lib newFunctionWithName:@"quad_fs"];
    NSData *bv = hs_sha_file_bytes(vs), *bf = hs_sha_file_bytes(fs);
    if (!vs || !fs || !bv.length || !bf.length) { printf("mtlprobe: FAIL hotswap: functions or bitcodeData missing\n"); return 2; }
    NSString *shaV = hs_sha(bv), *shaF = hs_sha(bf);
    NSFileManager *fm = [NSFileManager defaultManager];
    for (NSString *sha in @[ shaV, shaF ]) for (NSString *ext in @[ @".spv", @".meta.json" ])
        if (![fm fileExistsAtPath:[src stringByAppendingPathComponent:[sha stringByAppendingString:ext]]]) { printf("mtlprobe: FAIL hotswap: source %s/%s%s not found (the quad shaders need an spvcache entry to copy)\n", src.UTF8String, sha.UTF8String, ext.UTF8String); return 2; }
    [fm removeItemAtPath:@(side) error:NULL];
    if (mkdir(side, 0777) || chmod(side, 0777)) { printf("mtlprobe: FAIL hotswap: cannot create side dir %s\n", side); return 2; }
    printf("mtlprobe: hotswap: vertex sha %s fragment sha %s; side dir %s (empty); bundle spvcache hidden\n", shaV.UTF8String, shaF.UTF8String, side);
    MTLVertexDescriptor *vd = [MTLVertexDescriptor vertexDescriptor];
    vd.attributes[0].format = MTLVertexFormatFloat2; vd.attributes[0].offset = 0; vd.attributes[0].bufferIndex = 0;
    vd.attributes[1].format = MTLVertexFormatFloat2; vd.attributes[1].offset = 8; vd.attributes[1].bufferIndex = 0;
    vd.layouts[0].stride = 16; vd.layouts[0].stepFunction = MTLVertexStepFunctionPerVertex; vd.layouts[0].stepRate = 1;
    MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
    pd.vertexFunction = vs; pd.fragmentFunction = fs; pd.vertexDescriptor = vd; pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    NSError *err = nil; id<MTLRenderPipelineState> pso = [dev newRenderPipelineStateWithDescriptor:pd error:&err]; perr("newRenderPipelineState", err);
    int fails = 0;
    #define HS(cond, ...) do { int ok_ = (cond) ? 1 : 0; printf("mtlprobe: hotswap %s: ", ok_ ? "ok  " : "FAIL"); printf(__VA_ARGS__); printf("\n"); if (!ok_) fails++; } while (0)
    HS(pso != nil, "fallback pipeline object created for a spvcache miss (non-nil)");
    if (!pso) { [fm removeItemAtPath:@(side) error:NULL]; printf("mtlprobe: FAIL hotswap (no pipeline: RADV busy or refused?)\n"); return 1; }
    MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new];
    sd.minFilter = MTLSamplerMinMagFilterNearest; sd.magFilter = MTLSamplerMinMagFilterNearest; sd.sAddressMode = MTLSamplerAddressModeClampToEdge; sd.tAddressMode = MTLSamplerAddressModeClampToEdge;
    id<MTLSamplerState> ss = [dev newSamplerStateWithDescriptor:sd];
    id<MTLTexture> srcT = n_source_texture(dev, MTLPixelFormatRGBA8Unorm, N_MANAGED);
    id<MTLTexture> t1 = n_tex(dev, MTLPixelFormatBGRA8Unorm, W, H, MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead, MTLStorageModePrivate);
    id<MTLTexture> t2 = n_tex(dev, MTLPixelFormatBGRA8Unorm, W, H, MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead, MTLStorageModePrivate);
    float verts[16] = { -1, 1, 0, 0,   1, 1, 1, 0,   -1, -1, 0, 1,   1, -1, 1, 1 }; uint16_t idx[6] = { 0, 1, 2, 2, 1, 3 };
    id<MTLBuffer> vb = [dev newBufferWithLength:sizeof verts options:MTLResourceStorageModeShared], ib = [dev newBufferWithLength:sizeof idx options:MTLResourceStorageModeShared];
    if (!ss || !srcT || !t1 || !t2 || !vb || !ib) { printf("mtlprobe: FAIL hotswap alloc\n"); return 2; }
    memcpy(vb.contents, verts, sizeof verts); memcpy(ib.contents, idx, sizeof idx);
    id<MTLCommandQueue> q = [dev newCommandQueue];
    static uint8_t exp[W * H * 4]; hs_expected(exp);
    // 1: fallback draw
    id<MTLCommandBuffer> cb = hs_encode(q, pso, ss, srcT, t1, vb, ib);
    if (!n_run(cb, "hotswap fallback draw")) { printf("mtlprobe: FAIL hotswap: fallback command buffer\n"); return 2; }
    NSData *g = n_readback(dev, q, t1, W, H, 4); if (!g) return 2;
    int mg = hs_magenta(g.bytes), df = hs_diff(g.bytes, exp);
    HS(mg > 1000 && df > 1000, "before: fallback draw shows %d magenta pixels and %d pixels differ from the quad image (expected: magenta present, image wrong)", mg, df);
    // 2: a command buffer encoded with the fallback and held back
    id<MTLCommandBuffer> held = hs_encode(q, pso, ss, srcT, t2, vb, ib);
    // 3: install the translator's output
    double t0 = now_s();
    BOOL inst = hs_install(src, @(side), shaV, @".meta.json") && hs_install(src, @(side), shaF, @".meta.json") && hs_install(src, @(side), shaV, @".spv") && hs_install(src, @(side), shaF, @".spv");
    HS(inst, "installed <sha>.meta.json x2 then <sha>.spv x2 into %s by atomic rename", side);
    if (!inst) return 2;
    // 4: draw with the SAME pipeline object until the quad appears
    int swapped = 0; double tswap = 0; NSData *g2 = nil; int df2 = -1, tries = 0;
    while (now_s() - t0 < waitS) {
        usleep(100000); tries++;
        id<MTLCommandBuffer> c2 = hs_encode(q, pso, ss, srcT, t1, vb, ib);
        if (!n_run(c2, "hotswap probe draw")) { printf("mtlprobe: FAIL hotswap: draw after install\n"); return 2; }
        g2 = n_readback(dev, q, t1, W, H, 4); if (!g2) return 2;
        df2 = hs_diff(g2.bytes, exp);
        if (df2 == 0) { swapped = 1; tswap = now_s() - t0; break; }
    }
    HS(swapped, "after: the SAME pipeline object draws the exact quad image %.2f s after the install (%d draws; wait limit %.1f s; last diff %d pixels)", tswap, tries, waitS, df2);
    // 5: the held command buffer, committed after the swap
    int hst = n_run(held, "hotswap held (encoded with the fallback, committed after the swap)");
    HS(hst && held.status == MTLCommandBufferStatusCompleted, "command buffer encoded before the swap completes after it (status %ld)", (long)held.status);
    NSData *gh = n_readback(dev, q, t2, W, H, 4);
    if (gh) { int m3 = hs_magenta(gh.bytes); HS(m3 > 1000, "and it still shows the fallback (%d magenta pixels): in-flight work keeps the generation it was encoded with", m3); }
    // 6: a fresh command buffer after the swap
    id<MTLCommandBuffer> c3 = hs_encode(q, pso, ss, srcT, t1, vb, ib);
    n_run(c3, "hotswap final draw"); NSData *g3 = n_readback(dev, q, t1, W, H, 4);
    HS(g3 && hs_diff(g3.bytes, exp) == 0, "final draw after the swap is the exact quad image (%d pixels differ)", g3 ? hs_diff(g3.bytes, exp) : -1);
    [fm removeItemAtPath:@(side) error:NULL];
    printf(fails ? "mtlprobe: FAIL hotswap\n" : "mtlprobe: PASS hotswap\n"); return fails != 0;
}

// qccompute (native #12 follow-up): QuartzCore's average-luma compute pipelines, created the way CA::OGL::MetalContext::get_compute_pipeline does
// (newLibraryWithURL on QuartzCore's default.metallib, newFunctionWithName, MTLComputePipelineDescriptor + setComputeFunction:, then
// -newComputePipelineStateWithDescriptor:error: (CONFIRMED selector, QuartzCore 0x7ff80d2e6c74)). Also the other creators for comparison.
static int cmd_qccompute(uint64_t rid, int haveRid) {
    id<MTLDevice> dev = pick_device(rid, haveRid); if (!dev) return 2;
    NSError *err = nil;
    NSURL *u = [NSURL fileURLWithPath:@"/System/Library/Frameworks/QuartzCore.framework/Versions/A/Resources/default.metallib"];
    id<MTLLibrary> lib = [dev newLibraryWithURL:u error:&err]; perr("QuartzCore default.metallib", err);
    if (!lib) { printf("mtlprobe: FAIL qccompute library\n"); return 2; }
    const char *names[] = { "compute_average_luma", "compute_sum_luma", "tile_average_luma", "tile_average_luma_quad", "compute_apl", "compute_apl_fast", "sum_apl_and_compute_dimming_factor" };
    int fails = 0;
    SEL sd = NSSelectorFromString(@"newComputePipelineStateWithDescriptor:error:");
    printf("qccompute: device responds to newComputePipelineStateWithDescriptor:error: = %d\n", [(id)dev respondsToSelector:sd]);
    for (size_t i = 0; i < sizeof names / sizeof *names; i++) {
        id<MTLFunction> f = [lib newFunctionWithName:@(names[i])];
        if (!f) { printf("qccompute: %s: function NIL\n", names[i]); fails++; continue; }
        MTLComputePipelineDescriptor *d = [MTLComputePipelineDescriptor new]; d.computeFunction = f; d.label = [NSString stringWithFormat:@"CA %s", names[i]];
        err = nil; id p = nil;
        if ([(id)dev respondsToSelector:sd]) p = ((id (*)(id, SEL, id, NSError **))objc_msgSend)((id)dev, sd, d, &err);
        printf("qccompute: %s: descriptor:error: -> %s", names[i], p ? "non-nil" : "NIL"); if (err) printf(" (%s)", err.localizedDescription.UTF8String); printf("\n");
        if (!p) fails++;
        else printf("qccompute: %s:   maxTotalThreadsPerThreadgroup %lu threadExecutionWidth %lu staticThreadgroupMemoryLength %lu\n", names[i],
                    (unsigned long)[(id<MTLComputePipelineState>)p maxTotalThreadsPerThreadgroup], (unsigned long)[(id<MTLComputePipelineState>)p threadExecutionWidth], (unsigned long)[(id<MTLComputePipelineState>)p staticThreadgroupMemoryLength]);
        err = nil; id p2 = [dev newComputePipelineStateWithFunction:f error:&err];
        printf("qccompute: %s: function:error: -> %s\n", names[i], p2 ? "non-nil" : "NIL");
        err = nil; id p3 = [dev newComputePipelineStateWithDescriptor:d options:MTLPipelineOptionNone reflection:nil error:&err];
        printf("qccompute: %s: descriptor:options:reflection:error: -> %s\n", names[i], p3 ? "non-nil" : "NIL");
    }
    printf(fails ? "mtlprobe: FAIL qccompute (%d)\n" : "mtlprobe: PASS qccompute (%d)\n", fails);
    return fails != 0;
}

// qcluma: dispatches QuartzCore's compute_average_luma (default.metallib) on a small RGBA16Float texture and compares the per-threadgroup sums with a CPU
// reference derived from the kernel's SPIR-V (uniforms: ushort2 origin, ushort2 limit, 2x uint, 2x uchar; each of 64 threads sums 8 texels along x from
// (gid.x*8+origin.x, gid.y+origin.y) while inside the limit; thread 0 stores the group sum at out[(wg.y*groupsX)+wg.x]); the reference was checked on an Apple-silicon Mac first.
static int cmd_qcluma(uint64_t rid, int haveRid) {
    id<MTLDevice> dev = pick_device(rid, haveRid); if (!dev) return 2;
    NSError *err = nil;
    NSURL *u = [NSURL fileURLWithPath:@"/System/Library/Frameworks/QuartzCore.framework/Versions/A/Resources/default.metallib"];
    id<MTLLibrary> lib = [dev newLibraryWithURL:u error:&err]; perr("QuartzCore default.metallib", err);
    id<MTLFunction> f = lib ? [lib newFunctionWithName:@"compute_average_luma"] : nil;
    if (!f) { printf("mtlprobe: FAIL qcluma function\n"); return 2; }
    MTLComputePipelineDescriptor *d = [MTLComputePipelineDescriptor new]; d.computeFunction = f;
    SEL sd = NSSelectorFromString(@"newComputePipelineStateWithDescriptor:error:");
    id<MTLComputePipelineState> ps = ((id (*)(id, SEL, id, NSError **))objc_msgSend)((id)dev, sd, d, &err);
    printf("qcluma: pipeline via descriptor:error: %s\n", ps ? "non-nil" : "NIL"); perr("pipeline", err);
    if (!ps) { printf("mtlprobe: FAIL qcluma\n"); return 1; }
    enum { TW = 1024, TH = 4, GX = 2, GY = 4 };
    static __fp16 tex[TW * TH * 4];
    for (int y = 0; y < TH; y++) for (int x = 0; x < TW; x++) { __fp16 *p = tex + (y * TW + x) * 4; p[0] = (__fp16)(x % 7); p[1] = (__fp16)(y + 1); p[2] = (__fp16)((x + y) % 5); p[3] = (__fp16)1; }
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Float width:TW height:TH mipmapped:NO];
    td.usage = MTLTextureUsageShaderRead; td.storageMode = MTLStorageModeShared;
    id<MTLTexture> t = [dev newTextureWithDescriptor:td]; if (!t) { printf("mtlprobe: FAIL qcluma texture\n"); return 2; }
    [t replaceRegion:MTLRegionMake2D(0, 0, TW, TH) mipmapLevel:0 withBytes:tex bytesPerRow:TW * 8];
    struct __attribute__((packed)) { uint16_t ox, oy, lx, ly; uint32_t a, b; uint8_t c, e; } un = { 0, 0, 1000, 3, 0, 0, 0, 0 };   // 1000 of 1024 columns, 3 of 4 rows
    id<MTLBuffer> ob = [dev newBufferWithLength:GX * GY * 16 options:MTLResourceStorageModeShared]; memset(ob.contents, 0xEE, GX * GY * 16);
    id<MTLCommandQueue> q = [dev newCommandQueue]; id<MTLCommandBuffer> cb = [q commandBuffer];
    id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
    [ce setComputePipelineState:ps]; [ce setBytes:&un length:sizeof un atIndex:0]; [ce setBuffer:ob offset:0 atIndex:2]; [ce setTexture:t atIndex:0];
    [ce setThreadgroupMemoryLength:512 * 16 atIndex:0];
    [ce dispatchThreadgroups:MTLSizeMake(GX, GY, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
    [ce endEncoding];
    if (!n_run(cb, "qcluma")) { printf("mtlprobe: FAIL qcluma (command buffer)\n"); return 2; }
    const float *o = ob.contents; int bad = 0;
    for (int wy = 0; wy < GY; wy++) for (int wx = 0; wx < GX; wx++) {
        float e[4] = { 0, 0, 0, 0 };
        for (int th = 0; th < 64; th++) for (int k = 0; k < 8; k++) {
            int x = (wx * 64 + th) * 8 + un.ox + k, y = wy + un.oy; if (x >= un.lx || y >= un.ly) continue;   // the kernel stops advancing x at the limit but keeps looping; a texel is only read while inside
            const __fp16 *p = tex + (y * TW + x) * 4; for (int c = 0; c < 4; c++) e[c] += (float)p[c];
        }
        const float *g = o + (wy * GX + wx) * 4;
        int mism = memcmp(g, e, 16) != 0;
        printf("qcluma: group (%d,%d): got %g %g %g %g expected %g %g %g %g %s\n", wx, wy, g[0], g[1], g[2], g[3], e[0], e[1], e[2], e[3], mism ? "MISMATCH" : "ok");
        bad += mism;
    }
    printf(bad ? "mtlprobe: FAIL qcluma (%d of 8 groups differ)\n" : "mtlprobe: PASS qcluma (%d of 8 groups differ)\n", bad);
    return bad != 0;
}

int main(int argc, char **argv) {
    @autoreleasepool {
        setvbuf(stdout, NULL, _IOLBF, 0);
        if (argc < 2) { fprintf(stderr, "usage: mtlprobe list | buf|commit|clear [--registry-id N] | | triangle <out.png> [--registry-id N] [--precompiled [lib]] [--expected raw]\n"); return 64; }
        if (!strcmp(argv[1], "list")) return cmd_list();
        if (!strcmp(argv[1], "libhash")) {
            uint64_t rid = 0; int haveRid = 0; const char *path = NULL;
            for (int i = 2; i < argc; i++) {
                if (!strcmp(argv[i], "--registry-id") && i + 1 < argc) { rid = strtoull(argv[++i], NULL, 0); haveRid = 1; }
                else if (argv[i][0] != '-' && !path) path = argv[i];
                else { fprintf(stderr, "unknown arg %s\n", argv[i]); return 64; }
            }
            if (!path) { fprintf(stderr, "usage: mtlprobe libhash <path.metallib> [--registry-id N]\n"); return 64; }
            return cmd_libhash(path, rid, haveRid);
        }
        if (!strcmp(argv[1], "syspipe")) {
            uint64_t rid = 0; int haveRid = 0; const char *a[3] = {0}; int na = 0;
            for (int i = 2; i < argc; i++) {
                if (!strcmp(argv[i], "--registry-id") && i + 1 < argc) { rid = strtoull(argv[++i], NULL, 0); haveRid = 1; }
                else if (argv[i][0] != '-' && na < 3) a[na++] = argv[i];
                else { fprintf(stderr, "unknown arg %s\n", argv[i]); return 64; }
            }
            if (na != 3) { fprintf(stderr, "usage: mtlprobe syspipe <metallib> <vertexName> <fragmentName> [--registry-id N]\n"); return 64; }
            return cmd_syspipe(a[0], a[1], a[2], rid, haveRid);
        }
        if (!strcmp(argv[1], "apigaps")) {
            uint64_t rid = 0; int haveRid = 0; const char *only = NULL;
            for (int i = 2; i < argc; i++) { if (!strcmp(argv[i], "--registry-id") && i + 1 < argc) { rid = strtoull(argv[++i], NULL, 0); haveRid = 1; } else if (argv[i][0] != '-' && !only) only = argv[i]; else { fprintf(stderr, "unknown arg %s\n", argv[i]); return 64; } }
            return cmd_apigaps(rid, haveRid, only);
        }
        if (!strcmp(argv[1], "devcall") && argc >= 3) {
            uint64_t rid = 0; int haveRid = 0, hasArg = 0; unsigned long arg = 0;
            for (int i = 3; i < argc; i++) { if (!strcmp(argv[i], "--registry-id") && i + 1 < argc) { rid = strtoull(argv[++i], NULL, 0); haveRid = 1; } else { arg = strtoul(argv[i], NULL, 0); hasArg = 1; } }
            return cmd_devcall(argv[2], hasArg, arg, rid, haveRid);
        }
        if (!strcmp(argv[1], "formats") || !strcmp(argv[1], "tex1d") || !strcmp(argv[1], "cdpipe")) {
            uint64_t rid = 0; int haveRid = 0; const char *only = NULL;
            for (int i = 2; i < argc; i++) { if (!strcmp(argv[i], "--registry-id") && i + 1 < argc) { rid = strtoull(argv[++i], NULL, 0); haveRid = 1; } else if (argv[i][0] != '-' && !only) only = argv[i]; else { fprintf(stderr, "unknown arg %s\n", argv[i]); return 64; } }
            if (!strcmp(argv[1], "formats")) return cmd_formats(only ? only : "all", rid, haveRid);
            if (!strcmp(argv[1], "tex1d")) return cmd_tex1d(only, rid, haveRid);
            return cmd_cdpipe(only, rid, haveRid);
        }
        if (!strcmp(argv[1], "robust") && argc >= 3) {
            uint64_t rid = 0; int haveRid = 0;
            for (int i = 3; i < argc; i++) { if (!strcmp(argv[i], "--registry-id") && i + 1 < argc) { rid = strtoull(argv[++i], NULL, 0); haveRid = 1; } else { fprintf(stderr, "unknown arg %s\n", argv[i]); return 64; } }
            return cmd_robust(argv[2], rid, haveRid);
        }
        if (!strcmp(argv[1], "hotswap")) {
            uint64_t rid = 0; int haveRid = 0; double wait = 3.0; const char *srcd = NULL, *side = NULL;
            for (int i = 2; i < argc; i++) {
                if (!strcmp(argv[i], "--registry-id") && i + 1 < argc) { rid = strtoull(argv[++i], NULL, 0); haveRid = 1; }
                else if (!strcmp(argv[i], "--wait") && i + 1 < argc) wait = atof(argv[++i]);
                else if (!strcmp(argv[i], "--spv-src") && i + 1 < argc) srcd = argv[++i];
                else if (!strcmp(argv[i], "--side-dir") && i + 1 < argc) side = argv[++i];
                else { fprintf(stderr, "unknown arg %s\n", argv[i]); return 64; } }
            return cmd_hotswap(rid, haveRid, wait, srcd, side);
        }
        if (!strcmp(argv[1], "gaps2")) {
            uint64_t rid = 0; int haveRid = 0; const char *only = NULL;
            for (int i = 2; i < argc; i++) { if (!strcmp(argv[i], "--registry-id") && i + 1 < argc) { rid = strtoull(argv[++i], NULL, 0); haveRid = 1; } else if (argv[i][0] != '-' && !only) only = argv[i]; else { fprintf(stderr, "unknown arg %s\n", argv[i]); return 64; } }
            return cmd_gaps2(only, rid, haveRid);
        }
        if (!strcmp(argv[1], "nocopy")) {
            uint64_t rid = 0; int haveRid = 0; const char *only = (argc < 3 || argv[2][0] == '-') ? NULL : argv[2];
            for (int i = 2; i < argc; i++) if (!strcmp(argv[i], "--registry-id") && i + 1 < argc) { rid = strtoull(argv[++i], NULL, 0); haveRid = 1; }
            return cmd_nocopy(only, rid, haveRid);
        }
        if (!strcmp(argv[1], "initsel")) { uint64_t rid = 0; int haveRid = 0, ne = 0; char **ex = calloc((size_t)argc, sizeof(char *)); for (int i = 2; i < argc; i++) { if (!strcmp(argv[i], "--registry-id") && i + 1 < argc) { rid = strtoull(argv[++i], NULL, 0); haveRid = 1; } else ex[ne++] = argv[i]; } return cmd_initsel(rid, haveRid, ne, ex); }
        if (!strcmp(argv[1], "devcall2") && argc >= 4) {
            uint64_t rid = 0; int haveRid = 0, hasArg = 0; unsigned long arg = 0;
            for (int i = 4; i < argc; i++) { if (!strcmp(argv[i], "--registry-id") && i + 1 < argc) { rid = strtoull(argv[++i], NULL, 0); haveRid = 1; } else { arg = strtoul(argv[i], NULL, 0); hasArg = 1; } }
            return cmd_devcall2(argv[2], argv[3], hasArg, arg, rid, haveRid);
        }
        if (!strcmp(argv[1], "heap")) {
            uint64_t rid = 0; int haveRid = 0; const char *only = (argc < 3 || argv[2][0] == '-') ? NULL : argv[2];
            for (int i = 2; i < argc; i++) if (!strcmp(argv[i], "--registry-id") && i + 1 < argc) { rid = strtoull(argv[++i], NULL, 0); haveRid = 1; }
            return cmd_heap(only, rid, haveRid);
        }
        if (!strcmp(argv[1], "qccompute")) { uint64_t rid = 0; int haveRid = 0; for (int i = 2; i < argc; i++) if (!strcmp(argv[i], "--registry-id") && i + 1 < argc) { rid = strtoull(argv[++i], NULL, 0); haveRid = 1; } return cmd_qccompute(rid, haveRid); }
        if (!strcmp(argv[1], "qcluma")) { uint64_t rid = 0; int haveRid = 0; for (int i = 2; i < argc; i++) if (!strcmp(argv[i], "--registry-id") && i + 1 < argc) { rid = strtoull(argv[++i], NULL, 0); haveRid = 1; } return cmd_qcluma(rid, haveRid); }
        if (!strcmp(argv[1], "classdump")) return cmd_classdump(argc - 2, argv + 2);
        if (!strcmp(argv[1], "selcensus")) {
            uint64_t rid = 0; int haveRid = 0;
            for (int i = 2; i < argc; i++) { if (!strcmp(argv[i], "--registry-id") && i + 1 < argc) { rid = strtoull(argv[++i], NULL, 0); haveRid = 1; } else { fprintf(stderr, "unknown arg %s\n", argv[i]); return 64; } }
            return cmd_selcensus(rid, haveRid);
        }
        if (!strcmp(argv[1], "buf")) {
            uint64_t rid = 0; int haveRid = 0;
            for (int i = 2; i < argc; i++) {
                if (!strcmp(argv[i], "--registry-id") && i + 1 < argc) { rid = strtoull(argv[++i], NULL, 0); haveRid = 1; }
                else { fprintf(stderr, "unknown arg %s\n", argv[i]); return 64; }
            }
            return cmd_buf(rid, haveRid);
        }
        if (!strcmp(argv[1], "commit") || !strcmp(argv[1], "clear")) {
            uint64_t rid = 0; int haveRid = 0; const char *out = NULL;
            for (int i = 2; i < argc; i++) {
                if (!strcmp(argv[i], "--registry-id") && i + 1 < argc) { rid = strtoull(argv[++i], NULL, 0); haveRid = 1; }
                else if (argv[i][0] != '-') out = argv[i];
                else { fprintf(stderr, "unknown arg %s\n", argv[i]); return 64; }
            }
            return argv[1][1] == 'o' ? cmd_commit(rid, haveRid) : cmd_clear(rid, haveRid, out);
        }
        if (!strcmp(argv[1], "dispflip")) {
            uint64_t rid = 0; int haveRid = 0, secs = 10;
            for (int i = 2; i < argc; i++) {
                if (!strcmp(argv[i], "--registry-id") && i + 1 < argc) { rid = strtoull(argv[++i], NULL, 0); haveRid = 1; }
                else if (!strcmp(argv[i], "--seconds") && i + 1 < argc) secs = atoi(argv[++i]);
                else { fprintf(stderr, "usage: mtlprobe dispflip [--seconds N] [--registry-id N]\n"); return 64; }
            }
            if (secs < 1) secs = 1;
            return cmd_dispflip(rid, haveRid, secs);
        }
        if (!strcmp(argv[1], "frames") || !strcmp(argv[1], "timeout")) {
            uint64_t rid = 0; int haveRid = 0, N = !strcmp(argv[1], "frames") ? 1000 : 3000, inflight = 0;
            for (int i = 2; i < argc; i++) {
                if (!strcmp(argv[i], "--registry-id") && i + 1 < argc) { rid = strtoull(argv[++i], NULL, 0); haveRid = 1; }
                else if (!strcmp(argv[i], "--inflight") && i + 1 < argc) inflight = atoi(argv[++i]);
                else if (!strcmp(argv[i], "--passes") && i + 1 < argc) N = atoi(argv[++i]);
                else if (argv[i][0] != '-') N = atoi(argv[i]);
                else { fprintf(stderr, "unknown arg %s\n", argv[i]); return 64; }
            }
            return argv[1][0] == 'f' ? cmd_frames(rid, haveRid, N, inflight) : cmd_timeout(rid, haveRid, N);
        }
        if (!strcmp(argv[1], "quad") || !strcmp(argv[1], "blend") || !strcmp(argv[1], "compute") || !strcmp(argv[1], "sysdraw") || !strcmp(argv[1], "dumpair") || !strcmp(argv[1], "skydraw") || !strcmp(argv[1], "cull") || !strcmp(argv[1], "extra") || !strcmp(argv[1], "passbreak") || !strcmp(argv[1], "fbfetch") || !strcmp(argv[1], "fbsover") || !strcmp(argv[1], "fbcopy") || !strcmp(argv[1], "rndprobe") || !strcmp(argv[1], "fbwithin") || !strcmp(argv[1], "iosurface") || !strcmp(argv[1], "ioalias") || !strcmp(argv[1], "cube") || !strcmp(argv[1], "depth") || !strcmp(argv[1], "msaa") || !strcmp(argv[1], "rg16uint")) {
            uint64_t rid = 0; int haveRid = 0; const char *out = NULL; char defout[64];
            for (int i = 2; i < argc; i++) {
                if (!strcmp(argv[i], "--registry-id") && i + 1 < argc) { rid = strtoull(argv[++i], NULL, 0); haveRid = 1; }
                else if (argv[i][0] != '-' && !out) out = argv[i];
                else { fprintf(stderr, "unknown arg %s\n", argv[i]); return 64; }
            }
            if (!strcmp(argv[1], "dumpair")) { if (!out) { fprintf(stderr, "usage: mtlprobe dumpair <dir> [--registry-id N]\n"); return 64; } return cmd_dumpair(out, rid, haveRid); }
            if (!out) { snprintf(defout, sizeof defout, "%s-metal.png", argv[1]); out = defout; }
            if (!strcmp(argv[1], "cube")) return cmd_cube(rid, haveRid, out);
            if (!strcmp(argv[1], "depth")) return cmd_depth(rid, haveRid);
            if (!strcmp(argv[1], "msaa")) return cmd_msaa(rid, haveRid);
            if (!strcmp(argv[1], "rg16uint")) return cmd_rg16uint(rid, haveRid);
            if (!strcmp(argv[1], "iosurface")) return cmd_iosurface(rid, haveRid);
            if (!strcmp(argv[1], "ioalias")) return cmd_ioalias(rid, haveRid);
            if (!strcmp(argv[1], "fbwithin")) return cmd_fbwithin(rid, haveRid, out);
            if (!strcmp(argv[1], "rndprobe")) return cmd_rndprobe(rid, haveRid);
            if (!strcmp(argv[1], "fbfetch")) return cmd_fbfetch(rid, haveRid, out);
            if (!strcmp(argv[1], "fbsover")) return cmd_fbsover(rid, haveRid, out);
            if (!strcmp(argv[1], "fbcopy")) return cmd_fbcopy(rid, haveRid, out);
            if (!strcmp(argv[1], "quad")) return cmd_quad(rid, haveRid, out);
            if (!strcmp(argv[1], "blend")) return cmd_blend(rid, haveRid, out);
            if (!strcmp(argv[1], "compute")) return cmd_compute(rid, haveRid, out);
            if (!strcmp(argv[1], "skydraw")) return cmd_skydraw(rid, haveRid, out);
            if (!strcmp(argv[1], "cull")) return cmd_cull(rid, haveRid, out);
            if (!strcmp(argv[1], "passbreak")) return cmd_passbreak(rid, haveRid, out);
            if (!strcmp(argv[1], "extra")) return cmd_extra(rid, haveRid);
            return cmd_sysdraw(rid, haveRid, out);
        }
        if (!strcmp(argv[1], "triangle")) {
            static const char *defout = "triangle-metal.png";   // 10d: `mtlprobe triangle --registry-id N` needs no explicit path
            if (argc >= 3 && argv[2][0] == '-') { char **nv = malloc(sizeof(char *) * (argc + 2)); nv[0] = argv[0]; nv[1] = argv[1]; nv[2] = (char *)defout;
                for (int k = 2; k < argc; k++) nv[k + 1] = argv[k]; nv[argc + 1] = NULL; argv = nv; argc++; }
            if (argc < 3) { argv = (char *[]){ argv[0], argv[1], (char *)defout, NULL }; argc = 3; }
            uint64_t rid = 0; int haveRid = 0; BOOL pre = NO; const char *lib = "tri.metallib"; const char *exp = "expected.raw";
            // default expected.raw / metallib next to the executable
            static char expbuf[1024], libbuf[1024];
            NSString *dir = [[[NSProcessInfo processInfo] arguments][0] stringByDeletingLastPathComponent];
            if ([dir length] == 0) dir = @".";
            NSString *d = [[NSURL fileURLWithPath:dir] URLByResolvingSymlinksInPath].path;
            snprintf(expbuf, sizeof expbuf, "%s/expected.raw", [d UTF8String]); exp = expbuf;
            snprintf(libbuf, sizeof libbuf, "%s/tri.metallib", [d UTF8String]); lib = libbuf;
            for (int i = 3; i < argc; i++) {
                if (!strcmp(argv[i], "--registry-id") && i + 1 < argc) { rid = strtoull(argv[++i], NULL, 0); haveRid = 1; }
                else if (!strcmp(argv[i], "--precompiled")) { pre = YES; if (i + 1 < argc && argv[i+1][0] != '-') lib = argv[++i]; }
                else if (!strcmp(argv[i], "--expected") && i + 1 < argc) exp = argv[++i];
                else { fprintf(stderr, "unknown arg %s\n", argv[i]); return 64; }
            }
            return cmd_triangle(argv[2], rid, haveRid, pre, lib, exp);
        }
        fprintf(stderr, "unknown command %s\n", argv[1]); return 64;
    }
}
