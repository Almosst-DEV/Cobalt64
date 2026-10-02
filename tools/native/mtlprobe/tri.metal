#include <metal_stdlib>
using namespace metal;
vertex float4 vs(uint vid [[vertex_id]]) {
    float2 p[3] = { float2(-0.8, 0.8), float2(0.8, 0.8), float2(0.0, -0.8) };
    return float4(p[vid], 0.0, 1.0);
}
fragment float4 fs() { return float4(1.0, 0.8, 0.0, 1.0); }
