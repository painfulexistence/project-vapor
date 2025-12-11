#include <metal_stdlib>
using namespace metal;
#include "assets/shaders/3d_common.metal"

// ============================================================================
// CSM Depth Pass
// Renders depth from the light's perspective for each cascade
// ============================================================================

struct CSMVertexOut {
    float4 position [[position]];
    float2 uv;
};

// Push constants for per-cascade rendering
struct CSMPushConstants {
    uint cascadeIndex;
};

vertex CSMVertexOut vertexMain(
    uint vertexID [[vertex_id]],
    constant float4x4& lightViewProj [[buffer(0)]],
    constant InstanceData* instances [[buffer(1)]],
    device const VertexData* vertices [[buffer(2)]],
    constant uint& instanceID [[buffer(3)]]
) {
    CSMVertexOut out;

    uint actualVertexID = instances[instanceID].vertexOffset + vertexID;
    float4x4 model = instances[instanceID].model;
    float3 worldPos = (model * float4(vertices[actualVertexID].position, 1.0)).xyz;

    out.position = lightViewProj * float4(worldPos, 1.0);
    out.uv = float2(vertices[actualVertexID].uv);

    return out;
}

// Fragment shader for depth-only pass (optional alpha test)
fragment void fragmentMain(
    CSMVertexOut in [[stage_in]],
    texture2d<float, access::sample> texAlbedo [[texture(0), function_constant(hasAlbedoTexture)]]
) {
    // Alpha test for transparent objects
    constant bool hasAlbedoTexture = true;
    if (hasAlbedoTexture) {
        constexpr sampler s(address::repeat, filter::linear);
        float alpha = texAlbedo.sample(s, in.uv).a;
        if (alpha < 0.5) {
            discard_fragment();
        }
    }
    // Depth is automatically written
}

// Simplified depth-only fragment (no alpha test, for opaque geometry)
fragment void fragmentMainOpaque() {
    // Nothing to do - depth is written automatically
}
