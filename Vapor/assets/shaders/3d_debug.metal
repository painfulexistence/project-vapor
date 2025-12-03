#include <metal_stdlib>
using namespace metal;

// Debug draw vertex data (position + color)
struct DebugVertexIn {
    packed_float3 position;
    packed_float4 color;
};

// Camera data (same as 3d_common.metal)
struct CameraData {
    float4x4 proj;
    float4x4 view;
    float4x4 invProj;
    float4x4 invView;
    float near;
    float far;
    float3 position;
    float4 frustumPlanes[6];
};

// Rasterizer output
struct DebugVertexOut {
    float4 position [[position]];
    float4 color;
};

// Vertex shader for debug lines/triangles
vertex DebugVertexOut debug_vertex(
    uint vertexID [[vertex_id]],
    device const DebugVertexIn* vertices [[buffer(0)]],
    device const CameraData& camera [[buffer(1)]]
) {
    DebugVertexOut out;

    float3 worldPos = vertices[vertexID].position;
    out.position = camera.proj * camera.view * float4(worldPos, 1.0);
    out.color = float4(vertices[vertexID].color);

    return out;
}

// Fragment shader - simple pass-through color
fragment float4 debug_fragment(DebugVertexOut in [[stage_in]]) {
    return in.color;
}

// Alternative fragment shader with depth fade (optional)
fragment float4 debug_fragment_depth_fade(
    DebugVertexOut in [[stage_in]],
    float4 fragCoord [[position]]
) {
    // Fade color based on depth for better visibility
    float depth = fragCoord.z;
    float fade = 1.0 - smoothstep(0.0, 1.0, depth);
    float4 color = in.color;
    color.a *= fade;
    return color;
}
