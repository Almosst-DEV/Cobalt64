// test-intclear-semantics.m (bundle 19): runs Apple's REAL Metal (the host Mac's M4) and compares an RG16Uint render-pass Clear against n48_intfmt.h's n48if_clear_u, over edge values and a pseudo-random grid.
// Build/run (from this directory):  clang -fobjc-arc -framework Metal -framework Foundation -I. -o /tmp/test-intclear test-intclear-semantics.m && /tmp/test-intclear
// Exit 3 = SKIP (no Metal device). This is what proves the conversion is Metal's, not a guess.
#import <Metal/Metal.h>
#include <stdio.h>
#include <math.h>
#include "n48_intfmt.h"
int main(void) { @autoreleasepool {
    id<MTLDevice> dev = MTLCreateSystemDefaultDevice(); if (!dev) { printf("SKIP: no Metal device\n"); return 3; }
    id<MTLCommandQueue> q = [dev newCommandQueue];
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRG16Uint width:2 height:2 mipmapped:NO];
    td.usage = MTLTextureUsageRenderTarget; td.storageMode = MTLStorageModeShared;
    double edge[] = { 0, 1, 0.5, 0.9999999, 1.5, 2.5, 3.5, 254.5, 255, 255.9999, 256, 12345, 12345.99, 32767.5, 32768, 65534.5, 65535, 65535.5, 65535.9999, 65536, 70000, 1e10, 4294967295.0, 4294967296.0, 1e300,
                      -0.0, -1e-300, -0.5, -1, -5, -65535, -1e10, INFINITY, -INFINITY, NAN };
    int n = (int)(sizeof edge / sizeof *edge), tests = 0, bad = 0;
    unsigned long long s = 88172645463325252ull;
    for (int i = 0; i < n * n + 200; i++) {
        double a, b;
        if (i < n * n) { a = edge[i / n]; b = edge[i % n]; }
        else { s ^= s << 13; s ^= s >> 7; s ^= s << 17; a = (double)(s % 140000) / 8.0 - 2000.0; s ^= s << 13; s ^= s >> 7; s ^= s << 17; b = (double)(s % 140000) / 8.0 - 2000.0; }
        id<MTLTexture> t = [dev newTextureWithDescriptor:td]; if (!t) { printf("FAIL: no RG16Uint texture\n"); return 1; }
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = t; rp.colorAttachments[0].loadAction = MTLLoadActionClear; rp.colorAttachments[0].storeAction = MTLStoreActionStore; rp.colorAttachments[0].clearColor = MTLClearColorMake(a, b, 0, 0);
        id<MTLCommandBuffer> cb = [q commandBuffer]; id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:rp]; [e endEncoding]; [cb commit]; [cb waitUntilCompleted];
        uint16_t px[2]; [t getBytes:px bytesPerRow:8 fromRegion:MTLRegionMake2D(1, 1, 1, 1) mipmapLevel:0];
        unsigned ea = n48if_clear_u(a, 16), eb = n48if_clear_u(b, 16); tests++;
        if (px[0] != ea || px[1] != eb) { bad++; if (bad <= 10) printf("FAIL: clear (%.17g, %.17g): Metal (%u, %u), n48if_clear_u (%u, %u)\n", a, b, px[0], px[1], ea, eb); }
    }
    printf("%s: %d clears of an RG16Uint target on %s, %d differ from n48if_clear_u\n", bad ? "FAIL" : "ok  ", tests, dev.name.UTF8String, bad);
    return bad ? 1 : 0;
} }
