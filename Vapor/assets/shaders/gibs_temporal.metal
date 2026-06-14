#include <metal_stdlib>
#include "gibs_common.metal"
using namespace metal;

// ============================================================================
// GIBS Temporal Stability Shader
// Applies temporal filtering to reduce noise in GI result
//
// Design Decision: Temporal accumulation with motion-aware rejection
// - Blend current frame with history using exponential moving average
// - Reject history when motion exceeds threshold
// - Use variance-based clamping to reduce ghosting
// ============================================================================

// Sample history with bilinear interpolation
float4 sampleHistory(texture2d<float, access::sample> historyTex,
                      sampler s, float2 uv) {
    // Clamp to valid UV range
    uv = saturate(uv);
    return historyTex.sample(s, uv);
}

// Compute motion vector from current and previous view-projection
float2 computeMotionVector(float3 worldPos, float4x4 prevViewProj,
                            float4x4 invViewProj, float2 currentUV) {
    // Project to previous frame
    float4 prevClip = prevViewProj * float4(worldPos, 1.0);
    float2 prevUV = (prevClip.xy / prevClip.w) * 0.5 + 0.5;
    prevUV.y = 1.0 - prevUV.y; // Flip Y

    return currentUV - prevUV;
}

// Variance-based history clamping to reduce ghosting
float3 clampHistory(float3 history, float3 current, float3 neighborMin, float3 neighborMax) {
    // AABB clamp
    return clamp(history, neighborMin, neighborMax);
}

// Main temporal resolve kernel for GI buffer
kernel void giTemporalResolve(
    texture2d<float, access::read> currentGI [[texture(0)]],
    texture2d<float, access::sample> historyGI [[texture(1)]],
    texture2d<float, access::read> depthTexture [[texture(2)]],
    texture2d<float, access::write> outputGI [[texture(3)]],
    constant GIBSData& gibs [[buffer(0)]],
    sampler linearSampler [[sampler(0)]],
    uint2 gid [[thread_position_in_grid]]
) {
    if (gid.x >= uint(gibs.giResolution.x) || gid.y >= uint(gibs.giResolution.y)) {
        return;
    }

    float2 uv = (float2(gid) + 0.5) / gibs.giResolution;
    float2 screenUV = uv; // Assuming GI is same as screen for motion

    // Read current frame GI
    float4 current = currentGI.read(gid);

    // Read depth for motion calculation
    uint2 depthCoord = uint2(uv * gibs.screenSize);
    float depth = depthTexture.read(depthCoord).r;

    // Skip sky
    if (depth > 0.9999) {
        outputGI.write(current, gid);
        return;
    }

    // Reconstruct world position
    float3 worldPos = reconstructWorldPosition(screenUV, depth, gibs.invViewProj);

    // Compute motion vector
    float2 motion = computeMotionVector(worldPos, gibs.prevViewProj, gibs.invViewProj, screenUV);
    float2 historyUV = uv - motion * (gibs.giResolution / gibs.screenSize);

    // Check if history is valid (within screen bounds)
    bool historyValid = all(historyUV >= 0.0) && all(historyUV <= 1.0);

    // Motion magnitude for blend factor adjustment
    float motionMag = length(motion * gibs.screenSize);
    float motionFactor = saturate(1.0 - motionMag * 0.1); // Reduce history weight with motion

    if (!historyValid) {
        // No valid history, use current
        outputGI.write(current, gid);
        return;
    }

    // Sample history
    float4 history = sampleHistory(historyGI, linearSampler, historyUV);

    // Gather neighbor samples for variance clamping
    float3 neighborMin = current.rgb;
    float3 neighborMax = current.rgb;

    // 3x3 neighborhood
    for (int dy = -1; dy <= 1; dy++) {
        for (int dx = -1; dx <= 1; dx++) {
            if (dx == 0 && dy == 0) continue;

            int2 neighborCoord = int2(gid) + int2(dx, dy);
            if (neighborCoord.x >= 0 && neighborCoord.x < int(gibs.giResolution.x) &&
                neighborCoord.y >= 0 && neighborCoord.y < int(gibs.giResolution.y)) {
                float3 neighbor = currentGI.read(uint2(neighborCoord)).rgb;
                neighborMin = min(neighborMin, neighbor);
                neighborMax = max(neighborMax, neighbor);
            }
        }
    }

    // Expand bounds slightly to reduce flickering
    float3 boundsPadding = (neighborMax - neighborMin) * 0.25;
    neighborMin -= boundsPadding;
    neighborMax += boundsPadding;

    // Clamp history to neighborhood bounds
    float3 clampedHistory = clampHistory(history.rgb, current.rgb, neighborMin, neighborMax);

    // Blend factor with motion adjustment
    float blendFactor = gibs.temporalBlend + (1.0 - gibs.temporalBlend) * (1.0 - motionFactor);

    // Exponential moving average blend
    float3 result = mix(clampedHistory, current.rgb, blendFactor);

    outputGI.write(float4(result, 1.0), gid);
}

// Surfel-level temporal stability (applied during raytracing)
// This is called from surfel update to smooth irradiance over time
kernel void surfelTemporalSmooth(
    device Surfel* surfels [[buffer(0)]],
    constant GIBSData& gibs [[buffer(1)]],
    uint gid [[thread_position_in_grid]]
) {
    if (gid >= gibs.activeSurfelCount) {
        return;
    }

    Surfel surfel = surfels[gid];

    if (!(surfel.flags & SURFEL_FLAG_VALID)) {
        return;
    }

    // Hysteresis: gradually reduce irradiance if not updated
    // This handles surfels that are no longer visible
    if (!(surfel.flags & SURFEL_FLAG_NEEDS_UPDATE)) {
        float decayFactor = gibs.hysteresis;
        surfel.irradiance *= decayFactor;
    }

    surfels[gid] = surfel;
}

// Copy GI texture for ping-pong
kernel void giCopy(
    texture2d<float, access::read> source [[texture(0)]],
    texture2d<float, access::write> dest [[texture(1)]],
    uint2 gid [[thread_position_in_grid]]
) {
    uint2 size = uint2(source.get_width(), source.get_height());
    if (gid.x >= size.x || gid.y >= size.y) {
        return;
    }

    dest.write(source.read(gid), gid);
}
