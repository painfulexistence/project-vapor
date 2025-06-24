#include <metal_stdlib>
using namespace metal;

constant uint MAX_LIGHTS_PER_CLUSTER = 256;

struct Cluster {
    float4 min;
    float4 max;
    uint lightCount;
    uint lightIndices[MAX_LIGHTS_PER_CLUSTER];
};

struct PointLight {
    float3 position;
    float3 color;
    float intensity;
    float radius;
};

struct CameraData {
    float4x4 view;
    float4x4 proj;
    float4x4 invProj;
    float near;
    float far;
};

bool sphereAABBIntersection(float3 center, float radius, float3 aabbMin, float3 aabbMax) {
    float3 closestPoint = clamp(center, aabbMin, aabbMax);
    float distanceSquared = dot(closestPoint - center, closestPoint - center);
    return distanceSquared <= radius * radius;
}

kernel void computeMain(
    device Cluster* clusters [[buffer(0)]],
    const device PointLight* pointLights [[buffer(1)]],
    constant CameraData& camera [[buffer(2)]],
    constant uint& lightCount [[buffer(3)]],
    constant packed_uint3& gridSize [[buffer(4)]],
    uint3 gid [[threadgroup_position_in_grid]]
) {
    uint clusterIndex = gid.x + (gid.y * gridSize.x) + (gid.z * gridSize.x * gridSize.y);
    Cluster cluster = clusters[clusterIndex];
    cluster.lightCount = 0; // reset counter every frame

    for (uint i = 0; i < lightCount; ++i) {
        float3 center = float3(camera.view * float4(pointLights[i].position, 1.0));
        float radius = pointLights[i].radius;
        float3 aabbMin = cluster.min.xyz;
        float3 aabbMax = cluster.max.xyz;
        if (sphereAABBIntersection(center, radius, aabbMin, aabbMax) && cluster.lightCount < MAX_LIGHTS_PER_CLUSTER) {
            cluster.lightIndices[cluster.lightCount] = i;
            cluster.lightCount++;
        }
    }
    clusters[clusterIndex] = cluster;
}