#include <metal_stdlib>
using namespace metal;

// Pass 1: Count lights per cluster
// This shader counts how many lights affect each cluster without storing indices

struct ClusterCompact {
    float4 min;
    float4 max;
    uint offset;
    uint lightCount;
};

struct PointLight {
    float3 position;
    float _pad1;
    float3 color;
    float intensity;
    float radius;
    float3 _pad2;
};

struct CameraData {
    float4x4 proj;
    float4x4 view;
    float4x4 invProj;
    float4x4 invView;
    float near;
    float far;
};

bool sphereAABBIntersection(float3 center, float radius, float3 aabbMin, float3 aabbMax) {
    float3 closestPoint = clamp(center, aabbMin, aabbMax);
    float distanceSquared = dot(closestPoint - center, closestPoint - center);
    return distanceSquared <= radius * radius;
}

kernel void computeMain(
    device ClusterCompact* clusters [[buffer(0)]],
    const device PointLight* pointLights [[buffer(1)]],
    constant CameraData& camera [[buffer(2)]],
    constant uint& lightCount [[buffer(3)]],
    constant packed_uint3& gridSize [[buffer(4)]],
    uint3 gid [[threadgroup_position_in_grid]]
) {
    uint clusterIndex = gid.x + (gid.y * gridSize.x) + (gid.z * gridSize.x * gridSize.y);

    float3 aabbMin = clusters[clusterIndex].min.xyz;
    float3 aabbMax = clusters[clusterIndex].max.xyz;

    uint count = 0;
    for (uint i = 0; i < lightCount; ++i) {
        float3 center = float3(camera.view * float4(pointLights[i].position, 1.0));
        float radius = pointLights[i].radius;
        if (sphereAABBIntersection(center, radius, aabbMin, aabbMax)) {
            count++;
        }
    }

    clusters[clusterIndex].lightCount = count;
    // offset will be filled by prefix sum pass
}
