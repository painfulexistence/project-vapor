#include <metal_stdlib>
using namespace metal;
#include "Res/shaders/3d_common.metal"
#include "Res/shaders/3d_volumetric_common.metal"  // exponentialHeightFog, phaseHenyeyGreenstein

// Cheap analytic exponential height fog (RHI-Metal twin of HeightFog.frag): a
// single evaluation per pixel — no raymarch, no shadows, no per-light loops.
// The expensive volumetric variant is simpleFogFragment in 3d_volumetric_fog.metal.

constant float2 fsTriVerts[3] = { float2(-1, -1), float2(3, -1), float2(-1, 3) };

struct VertexOut {
    float4 position [[position]];
    float2 uv;
};

// vec4-packed to match the C++ HeightFogRenderData / GLSL std430 layout exactly.
struct HeightFogData {
    float4x4 invViewProj;
    float4 cameraPosition;     // xyz
    float4 sunDirection;       // xyz
    float4 sunColorIntensity;  // rgb + a=intensity
    float4 fogColorDensity;    // rgb + a=density
    float4 params;             // x=falloff y=baseHeight z=anisotropy w=ambient
};

vertex VertexOut heightFogVertex(uint vertexID [[vertex_id]]) {
    VertexOut out;
    out.position = float4(fsTriVerts[vertexID], 0.0, 1.0);
    out.uv = fsTriVerts[vertexID] * 0.5 + 0.5;
    out.uv.y = 1.0 - out.uv.y;  // Metal Y-down UV
    return out;
}

fragment float4 heightFogFragment(
    VertexOut in [[stage_in]],
    texture2d<float, access::sample> sceneColor [[texture(0)]],
    texture2d<float, access::sample> sceneDepth [[texture(1)]],
    constant HeightFogData& d [[buffer(0)]]
) {
    constexpr sampler linearSampler(filter::linear, address::clamp_to_edge);

    float4 color = sceneColor.sample(linearSampler, in.uv);
    float depth = sceneDepth.sample(linearSampler, in.uv).r;
    if (depth >= 0.9999) return color;  // skip sky

    float2 ndc = in.uv * 2.0 - 1.0;
    ndc.y = -ndc.y;  // Metal Y-flip (matches simpleFogFragment)
    float4 clipPos = float4(ndc, depth, 1.0);
    float4 worldPos4 = d.invViewProj * clipPos;
    float3 worldPos = worldPos4.xyz / worldPos4.w;

    float3 camPos = d.cameraPosition.xyz;
    float3 rayDir = normalize(worldPos - camPos);
    float dist = length(worldPos - camPos);
    float height = worldPos.y - d.params.y;   // relative to base height

    float fogAmount = saturate(exponentialHeightFog(dist, height, d.fogColorDensity.a, d.params.x));

    float phase = phaseHenyeyGreenstein(dot(rayDir, normalize(d.sunDirection.xyz)), d.params.z);
    float3 fogColor = d.sunColorIntensity.rgb * d.sunColorIntensity.a * phase * 0.1
                    + d.fogColorDensity.rgb * d.params.w;

    return float4(mix(color.rgb, fogColor, fogAmount), color.a);
}
