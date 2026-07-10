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
    float4x4 invView;
    float near;
    float far;
};

bool sphereTileIntersection(float3 center, float radius, float4x4 viewProj, float2 tileMin, float2 tileMax, float2 screenSize,
                            float sliceZ0, float sliceZ1) {
    float4 clipPos = viewProj * float4(center, 1.0);
    // clipPos.w is the view-space forward distance (+ in front of the camera,
    // - behind). A light whose influence sphere is entirely behind the camera
    // cannot touch a visible pixel — reject it (otherwise the flipped ndc below
    // scatters it into arbitrary tiles). Only spheres straddling the near/camera
    // plane get the conservative accept. Mirrors TileLightCull.comp.
    if (clipPos.w < -radius) { return false; }
    // 3D clustering: reject lights whose depth interval misses this z-slice.
    // clipPos.w IS the light's view-space depth, so the test needs no
    // projection and is valid even for near-plane-straddling lights — do it
    // BEFORE the conservative accept below, or near lights flood all Z slices
    // (an every-cluster floor, the depth-axis twin of the behind-camera bug).
    if (clipPos.w + radius < sliceZ0 || clipPos.w - radius > sliceZ1) { return false; }
    // Straddling the near plane: the projected position is unreliable, so
    // conservatively accept for every XY tile — but only in the admitted slices.
    if (clipPos.w <= radius) { return true; }

    float3 ndc = clipPos.xyz / clipPos.w;
    float2 screenUV = ndc.xy * 0.5 + 0.5;
    // screenUV.y = 1.0 - screenUV.y;

    float2 centerSS = screenUV * screenSize;
    float radiusSS = radius * 2.0 * min(screenSize.x, screenSize.y) / abs(clipPos.w * 2.0); // approximation (kept)
    // Half-tile pad against center/edge quantization (tile size = tileMax-tileMin).
    float2 pad = 0.5 * (tileMax - tileMin);
    float2 sphereMin = centerSS - radiusSS - pad;
    float2 sphereMax = centerSS + radiusSS + pad;

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
    // 3D cluster index: (x, y) screen tile x logarithmic depth slice z. The
    // slice mapping MUST match the readers (PBR / stochastic point shadow):
    //   slice k spans [near * (far/near)^(k/Z), near * (far/near)^((k+1)/Z))
    uint tileIndex = gid.x + gid.y * gridSize.x + gid.z * gridSize.x * gridSize.y;
    Cluster tile = tiles[tileIndex];
    tile.lightCount = 0; // reset counter every frame

    // Calculate tile bounding box in screen space
    // Tile(0, 0) is bottom-left
    float2 tileSize = screenSize / float2(gridSize.xy);
    float2 tileMin = float2(gid.xy) * tileSize;
    float2 tileMax = float2(gid.xy + 1) * tileSize;

    float zRatio = camera.far / camera.near;
    float sliceZ0 = camera.near * pow(zRatio, float(gid.z) / float(gridSize.z));
    float sliceZ1 = camera.near * pow(zRatio, float(gid.z + 1) / float(gridSize.z));

    float4x4 viewProj = camera.proj * camera.view;
    for (uint i = 0; i < lightCount; i++) {
        PointLight light = pointLights[i];
        if (sphereTileIntersection(light.position, light.radius, viewProj, tileMin, tileMax, screenSize, sliceZ0, sliceZ1) && tile.lightCount < MAX_LIGHTS_PER_TILE) {
            tile.lightIndices[tile.lightCount] = i;
            tile.lightCount++;
        }
    }

    tiles[tileIndex] = tile;
}