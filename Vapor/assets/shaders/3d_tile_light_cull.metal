#include <metal_stdlib>
using namespace metal;

constant uint MAX_LIGHTS_PER_TILE = 256; // Must match the definition in graphics.hpp
constant uint MAX_SPOTS_PER_CLUSTER = 64;
constant uint MAX_RECTS_PER_CLUSTER = 32;

struct Cluster {
    float4 min;
    float4 max;
    uint lightCount;
    uint lightIndices[MAX_LIGHTS_PER_TILE];
    uint spotCount;
    uint spotIndices[MAX_SPOTS_PER_CLUSTER];
    uint rectCount;
    uint rectIndices[MAX_RECTS_PER_CLUSTER];
};

// Matches Vapor::SpotLight (64B; MSL float3 slots pad to 16 like the C++ pads).
struct SpotLight {
    float3 position;
    float3 direction;
    float3 color;
    float radius;
    float cosInner;
    float cosOuter;
    float intensity;
};
// Matches Vapor::RectLight (64B tight — scalars ride the float3 tails, so
// packed_float3 is required here).
struct RectLight {
    packed_float3 position; float halfWidth;
    packed_float3 right;    float halfHeight;
    packed_float3 up;       float intensity;
    packed_float3 color;    uint useVideoTexture;
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
    const device SpotLight* spotLights [[buffer(6)]],
    const device RectLight* rectLights [[buffer(7)]],
    constant uint& spotLightCount [[buffer(8)]],
    constant uint& rectLightCountIn [[buffer(9)]],
    uint3 gid [[threadgroup_position_in_grid]],
    uint lid [[thread_index_in_threadgroup]]
) {
    // One threadgroup per 3D cluster, 64 threads cooperating (pipeline is
    // created with threadGroupSizeX = 64): each thread tests a strided slice
    // of the light lists and pushes hits through threadgroup atomics. The old
    // shape (1 thread/group, serial loop) idled 31/32 SIMD lanes.
    constexpr uint WG_SIZE = 64;
    threadgroup atomic_uint sPointCount;
    threadgroup atomic_uint sSpotCount;
    threadgroup atomic_uint sRectCount;
    threadgroup uint sPoint[MAX_LIGHTS_PER_TILE];
    threadgroup uint sSpot[MAX_SPOTS_PER_CLUSTER];
    threadgroup uint sRect[MAX_RECTS_PER_CLUSTER];
    // 3D cluster index: (x, y) screen tile x logarithmic depth slice z. The
    // slice mapping MUST match the readers (PBR / stochastic point shadow):
    //   slice k spans [near * (far/near)^(k/Z), near * (far/near)^((k+1)/Z))
    uint tileIndex = gid.x + gid.y * gridSize.x + gid.z * gridSize.x * gridSize.y;

    // Calculate tile bounding box in screen space
    // Tile(0, 0) is bottom-left
    float2 tileSize = screenSize / float2(gridSize.xy);
    float2 tileMin = float2(gid.xy) * tileSize;
    float2 tileMax = float2(gid.xy + 1) * tileSize;

    float zRatio = camera.far / camera.near;
    float sliceZ0 = camera.near * pow(zRatio, float(gid.z) / float(gridSize.z));
    float sliceZ1 = camera.near * pow(zRatio, float(gid.z + 1) / float(gridSize.z));

    if (lid == 0) {
        atomic_store_explicit(&sPointCount, 0u, memory_order_relaxed);
        atomic_store_explicit(&sSpotCount, 0u, memory_order_relaxed);
        atomic_store_explicit(&sRectCount, 0u, memory_order_relaxed);
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);

    float4x4 viewProj = camera.proj * camera.view;

    // Strided cooperative tests; hits compact through the shared counters.
    // List order becomes nondeterministic — every consumer is order-independent.
    for (uint i = lid; i < lightCount; i += WG_SIZE) {
        PointLight light = pointLights[i];
        if (sphereTileIntersection(light.position, light.radius, viewProj, tileMin, tileMax,
                                   screenSize, sliceZ0, sliceZ1)) {
            uint slot = atomic_fetch_add_explicit(&sPointCount, 1u, memory_order_relaxed);
            if (slot < MAX_LIGHTS_PER_TILE) sPoint[slot] = i;
        }
    }

    // Spot lights: range is a sphere radius, point test applies directly
    // (conservative for narrow cones; cone/AABB is the follow-up refinement).
    for (uint i = lid; i < spotLightCount; i += WG_SIZE) {
        SpotLight sl = spotLights[i];
        if (sphereTileIntersection(sl.position, sl.radius, viewProj, tileMin, tileMax,
                                   screenSize, sliceZ0, sliceZ1)) {
            uint slot = atomic_fetch_add_explicit(&sSpotCount, 1u, memory_order_relaxed);
            if (slot < MAX_SPOTS_PER_CLUSTER) sSpot[slot] = i;
        }
    }

    // Rect lights: no range field — conservative sphere = half-diagonal plus
    // the 1%-intensity falloff distance (mirrors TileLightCull.comp).
    for (uint i = lid; i < rectLightCountIn; i += WG_SIZE) {
        RectLight rl = rectLights[i];
        float halfDiag = length(float2(rl.halfWidth, rl.halfHeight));
        float range = halfDiag + sqrt(max(rl.intensity, 0.0) / 0.01);
        if (sphereTileIntersection(float3(rl.position), range, viewProj, tileMin, tileMax,
                                   screenSize, sliceZ0, sliceZ1)) {
            uint slot = atomic_fetch_add_explicit(&sRectCount, 1u, memory_order_relaxed);
            if (slot < MAX_RECTS_PER_CLUSTER) sRect[slot] = i;
        }
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);

    // Cooperative write-out of the compacted lists straight to device memory
    // (the old whole-Cluster local copy spilled ~1.4KB to stack per thread).
    uint pointsOut = min(atomic_load_explicit(&sPointCount, memory_order_relaxed), MAX_LIGHTS_PER_TILE);
    uint spotsOut  = min(atomic_load_explicit(&sSpotCount, memory_order_relaxed), MAX_SPOTS_PER_CLUSTER);
    uint rectsOut  = min(atomic_load_explicit(&sRectCount, memory_order_relaxed), MAX_RECTS_PER_CLUSTER);
    for (uint i = lid; i < pointsOut; i += WG_SIZE) tiles[tileIndex].lightIndices[i] = sPoint[i];
    for (uint i = lid; i < spotsOut;  i += WG_SIZE) tiles[tileIndex].spotIndices[i] = sSpot[i];
    for (uint i = lid; i < rectsOut;  i += WG_SIZE) tiles[tileIndex].rectIndices[i] = sRect[i];
    if (lid == 0) {
        tiles[tileIndex].lightCount = pointsOut;
        tiles[tileIndex].spotCount = spotsOut;
        tiles[tileIndex].rectCount = rectsOut;
    }
}