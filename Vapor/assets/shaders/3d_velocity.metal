// ============================================================================
// Velocity Buffer Pass
// ============================================================================
// Outputs per-pixel motion vectors in NDC space for:
// - Temporal Anti-Aliasing (TAA)
// - Motion Blur
// - Temporal Denoising (SVGF)
// - DLSS/FSR2/MetalFX
// - Temporal Volumetrics
// ============================================================================

#include "3d_common.metal"

struct VelocityVertexOut {
    float4 position [[position]];
    float4 currClipPos;
    float4 prevClipPos;
};

// ----------------------------------------------------------------------------
// Velocity Pass - Vertex Shader
// ----------------------------------------------------------------------------

vertex VelocityVertexOut velocityVertex(
    uint vertexID [[vertex_id]],
    constant CameraData& camera [[buffer(0)]],
    constant InstanceData* instances [[buffer(1)]],
    device const VertexData* vertices [[buffer(2)]],
    constant uint& instanceID [[buffer(3)]]
) {
    VelocityVertexOut out;

    uint actualVertexID = instances[instanceID].vertexOffset + vertexID;
    float3 localPos = vertices[actualVertexID].position;

    // Current frame position
    float4 worldPos = instances[instanceID].model * float4(localPos, 1.0);
    float4x4 viewProj = camera.proj * camera.view;
    out.currClipPos = viewProj * worldPos;
    out.position = out.currClipPos;

    // Previous frame position
    float4 prevWorldPos = instances[instanceID].prevModel * float4(localPos, 1.0);
    out.prevClipPos = camera.prevViewProj * prevWorldPos;

    return out;
}

// ----------------------------------------------------------------------------
// Velocity Pass - Fragment Shader
// ----------------------------------------------------------------------------
// Output: RG16Float texture
// R = velocity.x (NDC units, -2 to +2 range typical)
// G = velocity.y (NDC units, -2 to +2 range typical)

fragment float2 velocityFragment(
    VelocityVertexOut in [[stage_in]],
    constant CameraData& camera [[buffer(0)]]
) {
    // Convert to NDC (normalized device coordinates)
    float2 currNDC = in.currClipPos.xy / in.currClipPos.w;
    float2 prevNDC = in.prevClipPos.xy / in.prevClipPos.w;

    // Remove TAA jitter if present
    currNDC -= camera.jitter;
    prevNDC -= camera.prevJitter;

    // Velocity = current - previous (in NDC space)
    float2 velocity = currNDC - prevNDC;

    return velocity;
}

// ----------------------------------------------------------------------------
// Velocity Visualization - For Testing/Debug
// ----------------------------------------------------------------------------
// Converts velocity buffer to visible colors for debugging

struct FullscreenVertexOut {
    float4 position [[position]];
    float2 uv;
};

vertex FullscreenVertexOut velocityVisVertex(uint vertexID [[vertex_id]]) {
    FullscreenVertexOut out;

    // Fullscreen triangle
    float2 uv = float2((vertexID << 1) & 2, vertexID & 2);
    out.position = float4(uv * 2.0 - 1.0, 0.0, 1.0);
    out.uv = float2(uv.x, 1.0 - uv.y);

    return out;
}

fragment float4 velocityVisFragment(
    FullscreenVertexOut in [[stage_in]],
    texture2d<float, access::sample> velocityBuffer [[texture(0)]]
) {
    constexpr sampler linearSampler(filter::linear, address::clamp_to_edge);

    float2 velocity = velocityBuffer.sample(linearSampler, in.uv).rg;

    // Visualization options:

    // Option 1: Direct mapping (velocity as color)
    // Red = rightward motion, Cyan = leftward motion
    // Green = downward motion, Magenta = upward motion
    float3 color = float3(velocity * 0.5 + 0.5, 0.5);

    // Option 2: Magnitude as brightness, direction as hue
    // float mag = length(velocity) * 10.0; // Scale for visibility
    // float angle = atan2(velocity.y, velocity.x);
    // float3 color = float3(
    //     cos(angle) * 0.5 + 0.5,
    //     cos(angle + 2.094) * 0.5 + 0.5,  // 2π/3
    //     cos(angle + 4.189) * 0.5 + 0.5   // 4π/3
    // ) * saturate(mag);

    // Option 3: Motion lines (shows direction clearly)
    // float linePattern = fract(dot(velocity, in.uv * 100.0));
    // color = float3(linePattern);

    return float4(color, 1.0);
}

// ----------------------------------------------------------------------------
// Simple Motion Blur - Direct Test for Velocity Buffer
// ----------------------------------------------------------------------------

fragment float4 motionBlurFragment(
    FullscreenVertexOut in [[stage_in]],
    texture2d<float, access::sample> sceneColor [[texture(0)]],
    texture2d<float, access::sample> velocityBuffer [[texture(1)]],
    constant float& blurStrength [[buffer(0)]]  // Typically 0.5 to 2.0
) {
    constexpr sampler linearSampler(filter::linear, address::clamp_to_edge);

    float2 velocity = velocityBuffer.sample(linearSampler, in.uv).rg;

    // Convert NDC velocity to UV space
    float2 velocityUV = velocity * 0.5;  // NDC is -1 to 1, UV is 0 to 1
    velocityUV *= blurStrength;

    // Sample along motion direction
    const int numSamples = 8;
    float4 color = float4(0.0);

    for (int i = 0; i < numSamples; i++) {
        float t = float(i) / float(numSamples - 1) - 0.5;  // -0.5 to 0.5
        float2 sampleUV = in.uv + velocityUV * t;
        color += sceneColor.sample(linearSampler, sampleUV);
    }

    color /= float(numSamples);

    return color;
}

// ----------------------------------------------------------------------------
// Camera-Only Velocity (Fallback for static objects without prevModel)
// ----------------------------------------------------------------------------
// Uses depth buffer to reconstruct world position, then reprojects

fragment float2 velocityFromDepthFragment(
    FullscreenVertexOut in [[stage_in]],
    texture2d<float, access::sample> depthBuffer [[texture(0)]],
    constant CameraData& camera [[buffer(0)]]
) {
    constexpr sampler pointSampler(filter::nearest, address::clamp_to_edge);

    float depth = depthBuffer.sample(pointSampler, in.uv).r;

    // Reconstruct world position from depth
    float2 ndc = in.uv * 2.0 - 1.0;
    ndc.y = -ndc.y;  // Flip Y for Metal

    float4 clipPos = float4(ndc, depth, 1.0);
    float4 viewPos = camera.invProj * clipPos;
    viewPos /= viewPos.w;

    float4 worldPos = camera.invView * viewPos;

    // Reproject to previous frame
    float4 prevClipPos = camera.prevViewProj * worldPos;
    float2 prevNDC = prevClipPos.xy / prevClipPos.w;

    // Current NDC (with jitter removed)
    float2 currNDC = ndc - camera.jitter;
    prevNDC -= camera.prevJitter;

    return currNDC - prevNDC;
}
