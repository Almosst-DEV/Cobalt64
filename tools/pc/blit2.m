// Commit a blit, then STAY ALIVE so its buffers remain valid while the ring is
// rebased and kicked from another shell.: the previous probe was killed by an
// alarm, which freed src/dst and made replaying its ring entry unsafe.
#import <Metal/Metal.h>
#import <Foundation/Foundation.h>
int main(void) { @autoreleasepool {
    id<MTLDevice> d = MTLCreateSystemDefaultDevice();
    if (!d) { printf("no device\n"); return 1; }
    id<MTLCommandQueue> q = [d newCommandQueue];
    id<MTLBuffer> src = [d newBufferWithLength:4096 options:MTLResourceStorageModeShared];
    id<MTLBuffer> dst = [d newBufferWithLength:4096 options:MTLResourceStorageModeShared];
    if (!q || !src || !dst) { printf("setup failed\n"); return 1; }
    memset(src.contents, 0xAB, 4096);
    memset(dst.contents, 0x00, 4096);

    id<MTLCommandBuffer> cb = [q commandBuffer];
    id<MTLBlitCommandEncoder> be = [cb blitCommandEncoder];
    [be copyFromBuffer:src sourceOffset:0 toBuffer:dst destinationOffset:0 size:4096];
    [be endEncoding];

    printf("committing (buffers stay alive; poll /tmp/navi48-staging/blit2.state)\n"); fflush(stdout);
    FILE *f = fopen("/tmp/navi48-staging/blit2.state","w"); if (f) { fprintf(f,"committed\n"); fclose(f); }
    [cb commit];

    // Poll instead of blocking, so the process stays responsive and its buffers live.
    for (int i = 0; i < 3000; i++) {
        if (cb.status == MTLCommandBufferStatusCompleted ||
            cb.status == MTLCommandBufferStatusError) break;
        usleep(100000);
    }
    printf("status: %ld\n", (long)cb.status);
    if (cb.error) printf("error: %s\n", cb.error.localizedDescription.UTF8String);
    const unsigned char *p = (const unsigned char *)dst.contents;
    int ok = 1; for (int i = 0; i < 4096; i++) if (p[i] != 0xAB) { ok = 0; break; }
    printf("DATA: %s (dst[0]=0x%02x)\n", ok ? "COPIED CORRECTLY" : "not copied", p[0]);
    f = fopen("/tmp/navi48-staging/blit2.state","w");
    if (f) { fprintf(f,"done status=%ld data=%s\n",(long)cb.status, ok?"OK":"no"); fclose(f); }
    return 0;
} }
