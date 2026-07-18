#include <metal_stdlib>
using namespace metal;

// IBL debug: unwrap the environment cubemap into an equirectangular 2D image for
// ImGui. RHI-Metal twin of IblEquirect.frag; self-contained fullscreen triangle.

constant float2 fsVerts[3] = { float2(-1, -1), float2(3, -1), float2(-1, 3) };

struct VOut {
    float4 position [[position]];
    float2 uv;
};

vertex VOut vertexMain(uint vid [[vertex_id]]) {
    VOut o;
    o.position = float4(fsVerts[vid], 0.0, 1.0);
    o.uv = fsVerts[vid] * 0.5 + 0.5;
    o.uv.y = 1.0 - o.uv.y;
    return o;
}

fragment float4 fragmentMain(VOut in [[stage_in]],
                             texturecube<float, access::sample> envCubemap [[texture(0)]]) {
    constexpr sampler cubeSampler(filter::linear, mip_filter::linear);
    const float PI = 3.14159265359;
    float phi   = (in.uv.x * 2.0 - 1.0) * PI;
    float theta = in.uv.y * PI;
    float3 dir = float3(sin(theta) * sin(phi), cos(theta), -sin(theta) * cos(phi));
    // Sample mip 0 explicitly (see IblEquirectPreview.frag): auto-mip picks a
    // high/wrong mip for the fast azimuthal sweep and the sky repeats/breaks.
    return float4(envCubemap.sample(cubeSampler, dir, level(0)).rgb, 1.0);
}
