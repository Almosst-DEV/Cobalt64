// test-xlate-linked-real.m: writes the real AIR (MTLFunction.bitcodeData) of a fragment shader that calls a [[visible]] function of another library, and of that function, into the directory argv[1].
#import <Metal/Metal.h>
#import <Foundation/Foundation.h>
#import <objc/message.h>
int main(int argc, char **argv) { @autoreleasepool {
  id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
  NSString *a = @"#include <metal_stdlib>\nusing namespace metal;\n[[visible]] float4 shade(float4 c) { return c * 0.5f + float4(0.25f); }\n";
  NSString *b = @"#include <metal_stdlib>\nusing namespace metal;\n[[visible]] float4 shade(float4 c);\nvertex float4 vs(uint v [[vertex_id]]) { float2 p[3] = {float2(-1,-1), float2(3,-1), float2(-1,3)}; return float4(p[v], 0, 1); }\nfragment float4 fs(float4 pos [[position]]) { return shade(float4(1,0,0,1)); }\n";
  NSError *e = nil;
  MTLCompileOptions *o = [MTLCompileOptions new];
  id<MTLLibrary> la = [dev newLibraryWithSource:a options:o error:&e]; if (!la) { NSLog(@"A: %@", e); return 1; }
  id<MTLLibrary> lb = [dev newLibraryWithSource:b options:o error:&e]; if (!lb) { NSLog(@"B: %@", e); return 1; }
  id fa = [la newFunctionWithName:@"shade"], fv = [lb newFunctionWithName:@"vs"], ff = [lb newFunctionWithName:@"fs"];
  for (id f in @[fa, fv, ff]) { SEL s = NSSelectorFromString(@"bitcodeData"); BOOL r = [f respondsToSelector:s]; NSData *d = r ? ((NSData *(*)(id, SEL))objc_msgSend)(f, s) : nil; NSLog(@"%@: responds %d bitcode %lu", [f name], r, (unsigned long)d.length);
     if (d.length) [d writeToFile:[NSString stringWithFormat:@"%s/%@.air", argv[1], [f name]] atomically:YES]; }
  MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new]; pd.vertexFunction = fv; pd.fragmentFunction = ff; pd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
  MTLLinkedFunctions *lf = [MTLLinkedFunctions linkedFunctions]; lf.functions = @[fa]; pd.fragmentLinkedFunctions = lf;
  id<MTLRenderPipelineState> ps = [dev newRenderPipelineStateWithDescriptor:pd error:&e]; NSLog(@"pipeline %@ %@", ps, e);
  NSLog(@"fa class %@ ff class %@", [fa class], [ff class]);
  return 0; } }
