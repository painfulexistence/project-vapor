#include <metal_stdlib>
using namespace metal;

// Sun/lens flare — clean-room redesign, identical algorithm to SunFlare.frag
// (the GLSL twin). See that file for the design notes. Fullscreen triangle,
// additive over the HDR colorRT; sun occlusion from a 5-tap depth disk.
// Bindings through the RHI: buffer(0) = SunFlareRenderData, texture(0)/sampler(0)
// = scene depth.

struct FlareData {
    float2 sunScreenPos;   // uv, y-down (matches the vertex uv below)
    float2 aspectRatio;    // (w/h, 1)
    float3 sunColor;
    float intensity;
    float glowSize;
    float haloRadius;
    float ghostSpacing;
    float streakIntensity;
};

struct FlareVertexOut {
    float4 position [[position]];
    float2 uv;
};

constant float2 flareVerts[3] = { float2(-1.0, -1.0), float2(3.0, -1.0), float2(-1.0, 3.0) };

vertex FlareVertexOut vertexMain(uint vertexID [[vertex_id]]) {
    FlareVertexOut out;
    out.position = float4(flareVerts[vertexID], 0.0, 1.0);
    out.uv = flareVerts[vertexID] * 0.5 + 0.5;
    out.uv.y = 1.0 - out.uv.y;  // y-down uv, same convention as the GLSL twin
    return out;
}

static float softDisk(float2 p, float2 c, float r) {
    float d = length(p - c);
    return smoothstep(r, r * 0.35, d);
}

fragment float4 fragmentMain(
    FlareVertexOut in [[stage_in]],
    texture2d<float, access::sample> sceneDepth [[texture(0)]],
    sampler depthSampler [[sampler(0)]],
    constant FlareData& data [[buffer(0)]]
) {
    float2 sunUV = clamp(data.sunScreenPos, float2(0.001), float2(0.999));
    float vis = 0.0;
    const float2 taps[5] = {
        float2(0.0, 0.0), float2(0.008, 0.0), float2(-0.008, 0.0),
        float2(0.0, 0.008), float2(0.0, -0.008) };
    for (int i = 0; i < 5; i++) {
        float d = sceneDepth.sample(depthSampler, clamp(sunUV + taps[i], float2(0.001), float2(0.999))).r;
        vis += step(0.9999, d);
    }
    vis *= 0.2;

    float2 sp = data.sunScreenPos;
    float edge = smoothstep(-0.1, 0.1, sp.x) * smoothstep(1.1, 0.9, sp.x)
               * smoothstep(-0.1, 0.1, sp.y) * smoothstep(1.1, 0.9, sp.y);
    float I = data.intensity * vis * edge;
    if (I < 0.001) return float4(0.0);

    float2 uv = in.uv * data.aspectRatio;
    float2 sun = sp * data.aspectRatio;
    float2 center = float2(0.5) * data.aspectRatio;

    float3 flare = float3(0.0);

    // 1) Core glow
    {
        float d = length(uv - sun);
        flare += data.sunColor * exp(-(d * d) / (data.glowSize * data.glowSize)) * 2.0;
    }
    // 2) Anamorphic streak
    {
        float2 d = uv - sun;
        float streak = exp(-(d.y * d.y) / 0.0004) * exp(-(d.x * d.x) / 0.08);
        flare += float3(0.6, 0.75, 1.0) * data.sunColor * streak * data.streakIntensity;
    }
    // 3) Spectral halo ring
    {
        float d = length(uv - sun);
        float ring = exp(-pow((d - data.haloRadius) / 0.012, 2.0));
        float a = atan2(uv.y - sun.y, uv.x - sun.x);
        float3 tint = float3(0.9 + 0.1 * sin(a), 0.8 + 0.2 * sin(a + 2.1), 1.0);
        flare += tint * data.sunColor * ring * 0.15;
    }
    // 4) Chromatic ghosts along the sun->center axis
    {
        float2 axis = center - sun;
        for (int i = 1; i <= 4; i++) {
            float2 gpos = sun + axis * (data.ghostSpacing * float(i) * 1.35);
            float gsize = data.glowSize * (1.6 - float(i) * 0.25);
            float r = softDisk(uv, gpos, gsize * 1.06);
            float g = softDisk(uv, gpos, gsize);
            float b = softDisk(uv, gpos, gsize * 0.94);
            float fade = 1.0 - float(i - 1) * 0.22;
            flare += float3(r, g, b) * data.sunColor * (0.10 * fade);
        }
    }

    return float4(flare * I, 1.0);
}
