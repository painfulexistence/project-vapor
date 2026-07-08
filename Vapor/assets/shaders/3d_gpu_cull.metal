#include <metal_stdlib>
using namespace metal;

// GPU-driven rendering: frustum-cull compute kernel (Metal backend).
// Mirror of GpuCull.comp. One thread per instance; writes one indirect draw
// command per instance. Visible => instanceCount 1, culled => 0 (the indirect
// draw skips those). firstInstance carries the instance index so the vertex
// shader can look up InstanceData.

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

struct InstanceData {
    float4x4 model;
    float4 color;
    uint vertexOffset;
    uint indexOffset;
    uint vertexCount;
    uint indexCount;
    uint materialID;
    uint primitiveMode;
    float3 AABBMin;
    float3 AABBMax;
    float4 boundingSphere; // xyz = world center, w = radius
};

// Layout matches Vapor::DrawCommand and MTLDrawIndexedPrimitivesIndirectArguments
// (packed, 20 bytes).
struct DrawCommand {
    uint indexCount;
    uint instanceCount;
    uint firstIndex;
    int  vertexOffset;
    uint firstInstance;
};

// Entry point must be named computeMain (RHI Metal compute convention).
kernel void computeMain(
    device const CameraData&   cam           [[buffer(0)]],
    device const InstanceData* instances     [[buffer(1)]],
    device DrawCommand*        commands      [[buffer(2)]],
    constant uint&             instanceCount [[buffer(3)]],
    uint                       id            [[thread_position_in_grid]]
) {
    if (id >= instanceCount) {
        return;
    }

    InstanceData inst = instances[id];
    float3 center = inst.boundingSphere.xyz;
    float radius = inst.boundingSphere.w;

    bool visible = true;
    for (int i = 0; i < 6; ++i) {
        float4 plane = cam.frustumPlanes[i]; // normalized, points inward
        if (dot(plane.xyz, center) + plane.w < -radius) {
            visible = false;
            break;
        }
    }

    DrawCommand cmd;
    cmd.indexCount = inst.indexCount;
    cmd.instanceCount = visible ? 1u : 0u;
    cmd.firstIndex = inst.indexOffset;
    cmd.vertexOffset = int(inst.vertexOffset);
    cmd.firstInstance = id;
    commands[id] = cmd;
}
