#include <metal_stdlib>
using namespace metal;
#include "assets/shaders/3d_common.metal"

// Volumetric Light Scattering (God Rays) Pass
// Modern screen-space implementation with depth-aware ray marching
// Based on GPU Gems 3 technique with improvements:
// - Depth-aware occlusion sampling
// - Mie phase function for realistic scattering
// - Temporal stability support (via jitter)
// - Half-resolution option for performance

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
    float mieG;               // Mie scattering g parameter (-1 to 1)
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

// Henyey-Greenstein phase function
// Approximates Mie scattering angular distribution
// g < 0: backscattering, g = 0: isotropic, g > 0: forward scattering
float henyeyGreenstein(float cosTheta, float g) {
    float g2 = g * g;
    float denom = 1.0 + g2 - 2.0 * g * cosTheta;
    return (1.0 - g2) / (4.0 * PI * pow(max(denom, 0.0001), 1.5));
}

// Simplified Cornette-Shanks phase function (better for sun glare)
float cornetteShanks(float cosTheta, float g) {
    float g2 = g * g;
    float num = 3.0 * (1.0 - g2) * (1.0 + cosTheta * cosTheta);
    float denom = 2.0 * (2.0 + g2) * pow(1.0 + g2 - 2.0 * g * cosTheta, 1.5);
    return num / max(denom, 0.0001);
}

// Gold noise for temporal jitter
float goldNoise(float2 xy, float seed) {
    return fract(tan(distance(xy * 1.61803398874989484820459, xy) * seed) * xy.x);
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

    // Early out if sun is behind the camera (off-screen check with margin)
    // Allow some margin for rays to still be visible near screen edges
    float margin = 0.5;
    if (sunPos.x < -margin || sunPos.x > 1.0 + margin ||
        sunPos.y < -margin || sunPos.y > 1.0 + margin) {
        // Sun is too far off-screen, no god rays visible
        return float4(0.0, 0.0, 0.0, 0.0);
    }

    // Calculate ray from pixel to sun
    float2 deltaUV = sunPos - uv;
    float rayLength = length(deltaUV);

    // Limit ray distance
    rayLength = min(rayLength, data.maxDistance);

    // Normalize and scale delta for sampling
    float2 rayDir = normalize(deltaUV);
    float stepSize = rayLength / float(data.numSamples);
    float2 stepDelta = rayDir * stepSize;

    // Add temporal jitter to reduce banding
    float jitterOffset = data.jitter * goldNoise(uv * data.screenSize, frame.frameNumber * 0.1);
    float2 sampleUV = uv + stepDelta * jitterOffset;

    // Calculate angular attenuation based on view angle to sun
    // This creates the characteristic "ray" appearance
    float2 viewDir = normalize(uv - float2(0.5));
    float2 sunDir = normalize(sunPos - float2(0.5));
    float cosAngle = dot(viewDir, sunDir);

    // Apply phase function for realistic scattering appearance
    float phase = cornetteShanks(cosAngle, data.mieG);

    // Ray march towards sun, accumulating light
    float3 accumLight = float3(0.0);
    float illuminationDecay = 1.0;

    for (int i = 0; i < data.numSamples; i++) {
        // Bounds check
        if (sampleUV.x < 0.0 || sampleUV.x > 1.0 || sampleUV.y < 0.0 || sampleUV.y > 1.0) {
            sampleUV += stepDelta;
            illuminationDecay *= data.decay;
            continue;
        }

        // Sample depth to determine occlusion
        float sampleDepth = depthTexture.sample(linearSampler, sampleUV).r;

        // Only accumulate light from unoccluded samples (sky/far depth)
        // This creates the shadow ray effect - rays are blocked by geometry
        float occlusion = step(data.depthThreshold, sampleDepth);

        // Sample scene color for light contribution
        // Using color instead of just white gives colored god rays
        float3 sampleColor = colorTexture.sample(linearSampler, sampleUV).rgb;

        // Luminance-based contribution for cleaner rays
        float luminance = dot(sampleColor, float3(0.2126, 0.7152, 0.0722));
        float3 lightContrib = data.sunColor * luminance * data.sunIntensity;

        // Accumulate with decay
        accumLight += lightContrib * occlusion * illuminationDecay * data.weight;

        // Apply exponential decay
        illuminationDecay *= data.decay;

        // Step towards sun
        sampleUV += stepDelta;
    }

    // Apply density and exposure
    float3 godRays = accumLight * data.density * data.exposure * phase;

    // Distance-based falloff from sun position
    float distFromSun = length(uv - sunPos);
    float radialFalloff = 1.0 - saturate(distFromSun * 1.5);
    godRays *= radialFalloff;

    return float4(godRays, 1.0);
}
