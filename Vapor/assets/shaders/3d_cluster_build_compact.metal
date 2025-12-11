#include <metal_stdlib>
using namespace metal;

// Build cluster AABBs for the compact cluster structure

struct ClusterCompact {
    float4 min;
    float4 max;
    uint offset;
    uint lightCount;
};

struct CameraData {
    float4x4 proj;
    float4x4 view;
    float4x4 invProj;
    float4x4 invView;
    float near;
    float far;
};

float3 lineIntersectionWithZPlane(float3 start, float3 end, float zDistance) {
    float3 dir = end - start;
    float3 norm = float3(0.0, 0.0, -1.0);

    float t = (zDistance - dot(norm, start)) / abs(dot(norm, dir));
    return start + t * dir;
}

float3 screenToView(float2 screenUV, constant float4x4& invProj) {
    float4 ndc = float4(float2(screenUV.x, 1.0 - screenUV.y) * 2.0 - 1.0, 0.0, 1.0);
    float4 viewCoord = invProj * ndc;
    viewCoord /= viewCoord.w;
    return viewCoord.xyz;
}

kernel void computeMain(
    device ClusterCompact* clusters [[buffer(0)]],
    constant CameraData& camera [[buffer(1)]],
    constant float2& screenSize [[buffer(2)]],
    constant packed_uint3& gridSize [[buffer(3)]],
    uint3 gid [[threadgroup_position_in_grid]]
) {
    uint clusterIndex = gid.x + (gid.y * gridSize.x) + (gid.z * gridSize.x * gridSize.y);

    // Calculate view space AABB
    // Cluster(0, 0, 0) is bottom-left-near
    float2 tileSize = screenSize / float2(gridSize.xy);
    float2 minPointSS = float2(gid.xy) / float2(gridSize.xy);
    minPointSS.y = 1.0 - minPointSS.y;
    float2 maxPointSS = float2(gid.xy + 1) / float2(gridSize.xy);
    maxPointSS.y = 1.0 - maxPointSS.y;
    float3 minPointVS = screenToView(minPointSS, camera.invProj);
    float3 maxPointVS = screenToView(maxPointSS, camera.invProj);
    // Forward is Z-
    float planeNear = -camera.near * pow(camera.far / camera.near, gid.z / float(gridSize.z));
    float planeFar = -camera.near * pow(camera.far / camera.near, (gid.z + 1) / float(gridSize.z));
    float3 minPointNear = lineIntersectionWithZPlane(float3(0, 0, 0), minPointVS, planeNear);
    float3 minPointFar = lineIntersectionWithZPlane(float3(0, 0, 0), minPointVS, planeFar);
    float3 maxPointNear = lineIntersectionWithZPlane(float3(0, 0, 0), maxPointVS, planeNear);
    float3 maxPointFar = lineIntersectionWithZPlane(float3(0, 0, 0), maxPointVS, planeFar);
    float3 minPoint = min(minPointNear, minPointFar);
    float3 maxPoint = max(maxPointNear, maxPointFar);

    // Write cluster buffer
    clusters[clusterIndex].min = float4(minPoint, 0.0);
    clusters[clusterIndex].max = float4(maxPoint, 0.0);
    clusters[clusterIndex].offset = 0;
    clusters[clusterIndex].lightCount = 0;
}
