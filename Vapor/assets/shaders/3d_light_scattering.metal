#include <metal_stdlib>
using namespace metal;
#include "assets/shaders/3d_common.metal"

// Volumetric Light Scattering (God Rays) Pass
// Screen-space radial blur implementation based on GPU Gems 3
// Key features:
// - Depth-aware occlusion (rays blocked by geometry)
// - Sun visibility check (no rays when sun is fully occluded)
// - Radial blur from sun position

struct FrameData {
    uint frameNumber;
    float time;
    float deltaTime;
};

struct LightScatteringData {
    float2 sunScreenPos;      // Sun position in screen space [0,1]
    float2 screenSize;        // Screen dimensions
    float density;            // Scattering density (default: 1.0)
    float weight;             // Sample weight (default: 0.01)
    float decay;              // Exponential decay factor (default: 0.97)
    float exposure;           // Final exposure multiplier (default: 0.3)
    int numSamples;           // Number of samples per ray (default: 64)
    float maxDistance;        // Maximum ray distance in UV space (default: 1.0)
    float sunIntensity;       // Sun intensity multiplier
    float mieG;               // Unused in simplified version
    float3 sunColor;          // Sun color
    float _pad1;
    float depthThreshold;     // Depth threshold for occlusion (0.9999 = sky only)
    float jitter;             // Temporal jitter for stability (0-1)
    float2 _pad2;
};

// Full screen triangle vertices
constant float2 ndcVerts[3] = {
    float2(-1.0, -1.0),
    float2( 3.0, -1.0),
    float2(-1.0,  3.0)
};

struct VertexOut {
    float4 position [[position]];
    float2 uv;
};

// Simple hash for jitter
float hash(float2 p) {
    return fract(sin(dot(p, float2(127.1, 311.7))) * 43758.5453);
}

vertex VertexOut vertexMain(uint vertexID [[vertex_id]]) {
    VertexOut out;
    out.position = float4(ndcVerts[vertexID], 0.0, 1.0);
    out.uv = ndcVerts[vertexID] * 0.5 + 0.5;
    out.uv.y = 1.0 - out.uv.y; // Metal Y-flip
    return out;
}

fragment float4 fragmentMain(
    VertexOut in [[stage_in]],
    texture2d<float, access::sample> colorTexture [[texture(0)]],
    texture2d<float, access::sample> depthTexture [[texture(1)]],
    constant LightScatteringData& data [[buffer(0)]],
    constant FrameData& frame [[buffer(1)]]
) {
    constexpr sampler linearSampler(address::clamp_to_edge, filter::linear);

    float2 uv = in.uv;
    float2 sunPos = data.sunScreenPos;

    // Early out if sun is too far off-screen
    float margin = 0.3;
    if (sunPos.x < -margin || sunPos.x > 1.0 + margin ||
        sunPos.y < -margin || sunPos.y > 1.0 + margin) {
        return float4(0.0, 0.0, 0.0, 0.0);
    }

    // Check if sun itself is visible (not occluded by geometry)
    // Sample depth at sun position - if depth < threshold, sun is behind geometry
    float2 sunUVClamped = clamp(sunPos, float2(0.001), float2(0.999));
    float sunDepth = depthTexture.sample(linearSampler, sunUVClamped).r;
    float sunVisibility = step(data.depthThreshold, sunDepth);

    // If sun is completely occluded, no god rays
    if (sunVisibility < 0.01) {
        return float4(0.0, 0.0, 0.0, 0.0);
    }

    // Calculate ray direction from current pixel towards sun
    float2 deltaUV = sunPos - uv;
    float distToSun = length(deltaUV);

    // Skip if too close to sun position (avoid artifacts)
    if (distToSun < 0.001) {
        return float4(0.0, 0.0, 0.0, 0.0);
    }

    // Normalize direction and calculate step size
    float2 rayDir = deltaUV / distToSun;
    float stepLen = min(distToSun, data.maxDistance) / float(data.numSamples);
    float2 stepDelta = rayDir * stepLen;

    // Add temporal jitter to reduce banding
    float jitterAmount = data.jitter * hash(uv * data.screenSize + float2(frame.frameNumber));
    float2 sampleUV = uv + stepDelta * jitterAmount;

    // Ray march towards sun, accumulating light
    float3 accumLight = float3(0.0);
    float illuminationDecay = 1.0;

    for (int i = 0; i < data.numSamples; i++) {
        // Bounds check
        if (sampleUV.x < 0.0 || sampleUV.x > 1.0 || sampleUV.y < 0.0 || sampleUV.y > 1.0) {
            break; // Stop when leaving screen
        }

        // Sample depth - check if this point sees the sky
        float sampleDepth = depthTexture.sample(linearSampler, sampleUV).r;
        float isUnoccluded = step(data.depthThreshold, sampleDepth);

        // Sample scene color at this point
        float3 sampleColor = colorTexture.sample(linearSampler, sampleUV).rgb;

        // Use luminance of sky color as light contribution
        float luminance = dot(sampleColor, float3(0.2126, 0.7152, 0.0722));

        // Accumulate light with decay
        accumLight += data.sunColor * luminance * isUnoccluded * illuminationDecay * data.weight;

        // Apply exponential decay
        illuminationDecay *= data.decay;

        // Step towards sun
        sampleUV += stepDelta;
    }

    // Apply density, exposure, and sun visibility
    float3 godRays = accumLight * data.density * data.exposure * data.sunIntensity * sunVisibility;

    // Radial falloff - rays fade with distance from sun
    float falloff = 1.0 - saturate(distToSun * 0.8);
    falloff = falloff * falloff; // Quadratic falloff for smoother fade
    godRays *= falloff;

    return float4(godRays, 1.0);
}
