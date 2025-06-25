#include <metal_stdlib>
using namespace metal;

constant uint MAX_LIGHTS_PER_TILE = 256; // Must match the definition in graphics.hpp

struct Cluster {
    float4 min;
    float4 max;
    uint lightCount;
    uint lightIndices[MAX_LIGHTS_PER_TILE];
};

struct PointLight {
    float3 position;
    float3 color;
    float intensity;
    float radius;
};

struct CameraData {
    float4x4 proj;
    float4x4 view;
    float4x4 invProj;
    float near;
    float far;
};

bool sphereTileIntersection(float3 center, float radius, float4x4 viewProj, float2 tileMin, float2 tileMax, float2 screenSize) {
    float4 clipPos = viewProj * float4(center, 1.0);
    // if (clipPos.w <= 0.0) { // lights behind camera (but these lights can still affect the pixel if their radius is large enough)
    //     return false;
    // }
    float3 ndc = clipPos.xyz / clipPos.w;
    float2 screenUV = ndc.xy * 0.5 + 0.5;
    // screenUV.y = 1.0 - screenUV.y;

    float2 centerSS = screenUV * screenSize;
    float radiusSS = radius * 2.0 * min(screenSize.x, screenSize.y) / abs(clipPos.w * 2.0); // approximation
    float2 sphereMin = centerSS - radiusSS;
    float2 sphereMax = centerSS + radiusSS;

    return !(sphereMax.x < tileMin.x || sphereMin.x > tileMax.x ||
             sphereMax.y < tileMin.y || sphereMin.y > tileMax.y);
}

kernel void computeMain(
    device Cluster* tiles [[buffer(0)]],
    const device PointLight* pointLights [[buffer(1)]],
    constant CameraData& camera [[buffer(2)]],
    constant uint& lightCount [[buffer(3)]],
    constant packed_uint3& gridSize [[buffer(4)]],
    constant float2& screenSize [[buffer(5)]],
    uint3 gid [[threadgroup_position_in_grid]]
) {
    uint tileIndex = gid.x + gid.y * gridSize.x;
    Cluster tile = tiles[tileIndex];
    tile.lightCount = 0; // reset counter every frame

    // Calculate tile bounding box in screen space
    // Tile(0, 0) is bottom-left
    float2 tileSize = screenSize / float2(gridSize.xy);
    float2 tileMin = float2(gid.xy) * tileSize;
    float2 tileMax = float2(gid.xy + 1) * tileSize;

    float4x4 viewProj = camera.proj * camera.view;
    for (uint i = 0; i < lightCount; i++) {
        PointLight light = pointLights[i];
        if (sphereTileIntersection(light.position, light.radius, camera.proj * camera.view, tileMin, tileMax, screenSize) && tile.lightCount < MAX_LIGHTS_PER_TILE) {
            tile.lightIndices[tile.lightCount] = i;
            tile.lightCount++;
        }
    }

    tiles[tileIndex] = tile;
}