// test-occlusion-semantics.m: what Apple's Metal does with occlusion queries (setVisibilityResultMode:offset:, visibilityResultBuffer, visibilityResultType) - the measurements n48_occ.h is built on.
// Run on a Mac with real Metal (the host Mac, Apple M4, macOS 27.0.x:  clang -fobjc-arc -framework Metal -framework Foundation -o /tmp/occ test-occlusion-semantics.m && /tmp/occ
// Also runs against this bundle on the PC (it uses only public API on the default device). Measured on the host Mac: E1 [0]=12288 (3 draws of 4096, a Disabled draw adds nothing) [8]=1 [16]=0 [24]=0 (selected, no draw: WRITTEN 0)
// [32]=16 [40]=1, offsets never selected untouched; E2/E5 Reset overwrites (4096, then 0 / 4096 not 8192), Accumulate adds (sentinel+4096, sentinel+8192); E3 no mode ever set touches nothing; E4 the value is in the buffer
// when the completed handler runs; E6/E9 Accumulate+Boolean ORs (0|1=1, 4|1=5, hidden keeps 4); E7 a Boolean offset revisited with nothing visible stays 1; E8 Disabled on a fresh offset writes nothing
// (but a trailing Disabled@0 after a Boolean@8 wrote 0 at offset 0: unexplained, not copied). Boolean+Counting on one offset (E1 [48]) gave 2165: undefined.
#import <Metal/Metal.h>
#import <Foundation/Foundation.h>
static id<MTLDevice> dev; static id<MTLCommandQueue> q; static id<MTLRenderPipelineState> pso; static id<MTLTexture> tex;
static void dump(const char *tag, id<MTLBuffer> b, int n) {
    uint64_t *p = [b contents]; printf("%s:", tag);
    for (int i = 0; i < n; i++) printf(" [%d]=%llu", i * 8, (unsigned long long)p[i]); printf("\n");
}
static MTLRenderPassDescriptor *rpd(id<MTLBuffer> vb, int type) {
    MTLRenderPassDescriptor *d = [MTLRenderPassDescriptor renderPassDescriptor];
    d.colorAttachments[0].texture = tex; d.colorAttachments[0].loadAction = MTLLoadActionClear; d.colorAttachments[0].storeAction = MTLStoreActionStore;
    d.visibilityResultBuffer = vb;
    if (type >= 0 && [d respondsToSelector:@selector(setVisibilityResultType:)]) d.visibilityResultType = (MTLVisibilityResultType)type;
    return d;
}
int main(void) { @autoreleasepool {
    dev = MTLCreateSystemDefaultDevice(); q = [dev newCommandQueue];
    NSString *src = @"#include <metal_stdlib>\nusing namespace metal;\n"
      "vertex float4 vs(uint v [[vertex_id]], constant float2 *o [[buffer(0)]]) { float2 p[3] = {float2(-1,-1), float2(3,-1), float2(-1,3)}; return float4(p[v] + o[0], 0, 1); }\n"
      "fragment float4 fs() { return float4(1); }\n";
    NSError *e = nil; id<MTLLibrary> lib = [dev newLibraryWithSource:src options:nil error:&e]; if (!lib) { NSLog(@"lib %@", e); return 1; }
    MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new]; pd.vertexFunction = [lib newFunctionWithName:@"vs"]; pd.fragmentFunction = [lib newFunctionWithName:@"fs"];
    pd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm; pso = [dev newRenderPipelineStateWithDescriptor:pd error:&e]; if (!pso) { NSLog(@"pso %@", e); return 1; }
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:64 height:64 mipmapped:NO]; td.usage = MTLTextureUsageRenderTarget; td.storageMode = MTLStorageModePrivate; tex = [dev newTextureWithDescriptor:td];
    printf("device %s\n", [[dev name] UTF8String]);
    float on[2] = {0, 0}, off[2] = {10, 10};
    #define DRAW(e, o) do { [e setVertexBytes:(o) length:8 atIndex:0]; [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3]; } while (0)
    // E1: all within one pass
    id<MTLBuffer> vb = [dev newBufferWithLength:128 options:MTLResourceStorageModeShared]; memset([vb contents], 0xAB, 128);
    id<MTLCommandBuffer> cb = [q commandBuffer]; id<MTLRenderCommandEncoder> en = [cb renderCommandEncoderWithDescriptor:rpd(vb, 0)]; [en setRenderPipelineState:pso];
    [en setVisibilityResultMode:MTLVisibilityResultModeCounting offset:0]; DRAW(en, on); DRAW(en, on);               // [0] two draws, same mode/offset
    [en setVisibilityResultMode:MTLVisibilityResultModeBoolean offset:8]; DRAW(en, on);                              // [8] boolean visible
    [en setVisibilityResultMode:MTLVisibilityResultModeBoolean offset:16]; DRAW(en, off);                            // [16] boolean, nothing visible
    [en setVisibilityResultMode:MTLVisibilityResultModeCounting offset:24];                                          // [24] mode set, no draw
    [en setVisibilityResultMode:MTLVisibilityResultModeCounting offset:32]; [en setScissorRect:(MTLScissorRect){0,0,4,4}]; DRAW(en, on); [en setScissorRect:(MTLScissorRect){0,0,64,64}]; // [32] 16
    [en setVisibilityResultMode:MTLVisibilityResultModeDisabled offset:0]; DRAW(en, on);                              // disabled draw: must not add to [0]
    [en setVisibilityResultMode:MTLVisibilityResultModeCounting offset:0]; DRAW(en, on);                              // back to offset 0 after a disable: adds or overwrites?
    [en setVisibilityResultMode:MTLVisibilityResultModeBoolean offset:40]; DRAW(en, off); DRAW(en, on);               // [40] boolean: first not visible, then visible
    [en setVisibilityResultMode:MTLVisibilityResultModeBoolean offset:48]; DRAW(en, on); [en setVisibilityResultMode:MTLVisibilityResultModeCounting offset:48]; DRAW(en, on); // [48] boolean then counting at same offset
    // offsets 56.. never referenced
    [en endEncoding]; [cb commit]; [cb waitUntilCompleted]; dump("E1 (64x64; expect [0]=4096*3?)", vb, 8);
    // E2: pass-start Reset clears the whole buffer? (sentinel at [56] kept?) and Accumulate
    for (int ty = 0; ty <= 1; ty++) {
        id<MTLBuffer> v2 = [dev newBufferWithLength:64 options:MTLResourceStorageModeShared]; memset([v2 contents], 0xAB, 64);
        for (int pass = 0; pass < 2; pass++) {
            id<MTLCommandBuffer> c2 = [q commandBuffer]; id<MTLRenderCommandEncoder> e2 = [c2 renderCommandEncoderWithDescriptor:rpd(v2, ty)]; [e2 setRenderPipelineState:pso];
            [e2 setVisibilityResultMode:MTLVisibilityResultModeCounting offset:0]; if (pass == 0) DRAW(e2, on); else DRAW(e2, off);
            [e2 endEncoding]; [c2 commit]; [c2 waitUntilCompleted];
            char t[64]; snprintf(t, sizeof t, "E2 type=%d pass%d (pass0 visible 4096, pass1 offscreen)", ty, pass); dump(t, v2, 4);
        }
        // pass 2: a different buffer region, type same
    }
    // E3: pass with visibility buffer but never any setVisibilityResultMode
    { id<MTLBuffer> v3 = [dev newBufferWithLength:64 options:MTLResourceStorageModeShared]; memset([v3 contents], 0xAB, 64);
      id<MTLCommandBuffer> c3 = [q commandBuffer]; id<MTLRenderCommandEncoder> e3 = [c3 renderCommandEncoderWithDescriptor:rpd(v3, 0)]; [e3 setRenderPipelineState:pso]; DRAW(e3, on); [e3 endEncoding]; [c3 commit]; [c3 waitUntilCompleted]; dump("E3 no mode ever set", v3, 4); }
    // E4: contents written before completion handler? read inside addCompletedHandler
    { id<MTLBuffer> v4 = [dev newBufferWithLength:64 options:MTLResourceStorageModeShared]; memset([v4 contents], 0xAB, 64);
      id<MTLCommandBuffer> c4 = [q commandBuffer]; id<MTLRenderCommandEncoder> e4 = [c4 renderCommandEncoderWithDescriptor:rpd(v4, 0)]; [e4 setRenderPipelineState:pso];
      [e4 setVisibilityResultMode:MTLVisibilityResultModeBoolean offset:0]; DRAW(e4, on); [e4 endEncoding];
      [c4 addCompletedHandler:^(id<MTLCommandBuffer> c) { dump("E4 inside completed handler", v4, 2); }]; [c4 commit]; [c4 waitUntilCompleted]; }
    // E5: two passes in one command buffer, same offset, Reset vs Accumulate
    for (int ty = 0; ty <= 1; ty++) {
      id<MTLBuffer> v5 = [dev newBufferWithLength:64 options:MTLResourceStorageModeShared]; memset([v5 contents], 0xAB, 64);
      id<MTLCommandBuffer> c5 = [q commandBuffer];
      for (int p = 0; p < 2; p++) { id<MTLRenderCommandEncoder> e5 = [c5 renderCommandEncoderWithDescriptor:rpd(v5, ty)]; [e5 setRenderPipelineState:pso]; [e5 setVisibilityResultMode:MTLVisibilityResultModeCounting offset:0]; DRAW(e5, on); [e5 endEncoding]; }
      [c5 commit]; [c5 waitUntilCompleted]; char t[64]; snprintf(t, sizeof t, "E5 type=%d two passes same cb", ty); dump(t, v5, 3); }
    // E6: Disabled on a fresh offset; Accumulate+Boolean across two passes (zeroed buffer); Boolean-then-Counting alone
    { id<MTLBuffer> v6 = [dev newBufferWithLength:64 options:MTLResourceStorageModeShared]; memset([v6 contents], 0, 64);
      id<MTLCommandBuffer> c6 = [q commandBuffer];
      for (int p = 0; p < 2; p++) { id<MTLRenderCommandEncoder> e6 = [c6 renderCommandEncoderWithDescriptor:rpd(v6, 1)]; [e6 setRenderPipelineState:pso];
        [e6 setVisibilityResultMode:MTLVisibilityResultModeBoolean offset:0]; DRAW(e6, on);
        [e6 setVisibilityResultMode:MTLVisibilityResultModeDisabled offset:16]; DRAW(e6, on);
        [e6 setVisibilityResultMode:MTLVisibilityResultModeCounting offset:24]; DRAW(e6, off);
        [e6 endEncoding]; }
      [c6 commit]; [c6 waitUntilCompleted]; dump("E6 Accumulate zeroed: [0] boolean x2 passes, [16] disabled, [24] counting offscreen", v6, 4); }
    { id<MTLBuffer> v7 = [dev newBufferWithLength:64 options:MTLResourceStorageModeShared]; memset([v7 contents], 0xFF, 64);
      id<MTLCommandBuffer> c7 = [q commandBuffer]; id<MTLRenderCommandEncoder> e7 = [c7 renderCommandEncoderWithDescriptor:rpd(v7, 0)]; [e7 setRenderPipelineState:pso];
      [e7 setVisibilityResultMode:MTLVisibilityResultModeBoolean offset:0]; DRAW(e7, on); DRAW(e7, on); [e7 setVisibilityResultMode:MTLVisibilityResultModeBoolean offset:8]; DRAW(e7, on); [e7 setVisibilityResultMode:MTLVisibilityResultModeBoolean offset:0]; DRAW(e7, off);
      [e7 endEncoding]; [c7 commit]; [c7 waitUntilCompleted]; dump("E7 boolean x2 same interval, then revisit offset 0 with nothing visible (Reset)", v7, 3); }
    { id<MTLBuffer> v8 = [dev newBufferWithLength:64 options:MTLResourceStorageModeShared]; memset([v8 contents], 0xFF, 64);
      id<MTLCommandBuffer> c8 = [q commandBuffer]; id<MTLRenderCommandEncoder> e8 = [c8 renderCommandEncoderWithDescriptor:rpd(v8, 0)]; [e8 setRenderPipelineState:pso];
      [e8 setVisibilityResultMode:MTLVisibilityResultModeDisabled offset:16]; DRAW(e8, on); [e8 setVisibilityResultMode:MTLVisibilityResultModeBoolean offset:8]; DRAW(e8, on); [e8 setVisibilityResultMode:MTLVisibilityResultModeDisabled offset:0];
      [e8 endEncoding]; [c8 commit]; [c8 waitUntilCompleted]; dump("E8 Disabled fresh offset 16, boolean at 8, trailing Disabled@0 (sentinel -1)", v8, 3); }
    { id<MTLBuffer> v9 = [dev newBufferWithLength:64 options:MTLResourceStorageModeShared]; uint64_t *w = [v9 contents]; for (int k = 0; k < 8; k++) w[k] = 4;
      id<MTLCommandBuffer> c9 = [q commandBuffer]; id<MTLRenderCommandEncoder> e9 = [c9 renderCommandEncoderWithDescriptor:rpd(v9, 1)]; [e9 setRenderPipelineState:pso];
      [e9 setVisibilityResultMode:MTLVisibilityResultModeBoolean offset:0]; DRAW(e9, on); [e9 setVisibilityResultMode:MTLVisibilityResultModeBoolean offset:8]; DRAW(e9, off); [e9 setVisibilityResultMode:MTLVisibilityResultModeCounting offset:16]; DRAW(e9, off);
      [e9 endEncoding]; [c9 commit]; [c9 waitUntilCompleted]; dump("E9 Accumulate, buffer preset 4: boolean visible@0, boolean hidden@8, counting hidden@16", v9, 3); }
    return 0; } }
