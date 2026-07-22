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

// View-space AABB of this froxel (screen tile x depth slice). Corners are
// exact in view space: ndc.xy at forward distance z is
// (ndc.x*z/proj00, ndc.y*z/proj11, -z) — camera looks down -Z; dividing by the
// SIGNED proj[1][1] keeps the mapping convention-agnostic. The AABB bounds the
// frustum-slice shape, so tests against it are conservative: over-include only,
// never a false negative. Replaces the old screen-space projected-sphere test
// entirely — with it go its two failure modes (the under-estimated projected
// radius that caused hard tile-edge lighting seams, and the behind-camera /
// near-plane-straddle special cases, which a view-space distance test simply
// doesn't have). Mirrors TileLightCull.comp.
void froxelViewAABB(constant CameraData& camera, float2 screenSize,
                    float2 tileMin, float2 tileMax, float z0, float z1,
                    thread float3& mn, thread float3& mx) {
    float2 ndcMin = tileMin / screenSize * 2.0 - 1.0;
    float2 ndcMax = tileMax / screenSize * 2.0 - 1.0;
    float ip00 = 1.0 / camera.proj[0][0];
    float ip11 = 1.0 / camera.proj[1][1];
    mn = float3(1e30); mx = float3(-1e30);
    for (int c = 0; c < 8; ++c) {
        float nx = ((c & 1) == 0) ? ndcMin.x : ndcMax.x;
        float ny = ((c & 2) == 0) ? ndcMin.y : ndcMax.y;
        float z  = ((c & 4) == 0) ? z0 : z1;
        float3 pv = float3(nx * z * ip00, ny * z * ip11, -z);
        mn = min(mn, pv);
        mx = max(mx, pv);
    }
}

// Sphere vs AABB: squared distance from the closest point on the box.
bool sphereAABBIntersect(float3 center, float radius, float3 mn, float3 mx) {
    float3 d = center - clamp(center, mn, mx);
    return dot(d, d) <= radius * radius;
}

// Cone (spot) vs sphere — the "cull cone against sphere" test (Bart Wronski /
// Frostbite): angle cull, front (past range) cull, back (behind apex) cull.
// dir points FROM the light; cosHalf is the OUTER half-angle cosine.
bool spotConeIntersectsSphere(float3 origin, float3 dir, float range, float cosHalf,
                              float3 sc, float sr) {
    float sinHalf = sqrt(max(0.0, 1.0 - cosHalf * cosHalf));
    float3 V = sc - origin;
    float Vlen = length(V);
    float V1 = dot(V, dir);
    float distClosest = cosHalf * sqrt(max(0.0, Vlen * Vlen - V1 * V1)) - V1 * sinHalf;
    bool angleCull = distClosest > sr;
    bool frontCull = V1 > sr + range;
    bool backCull  = V1 < -sr;
    return !(angleCull || frontCull || backCull);
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

    // Froxel bounds, computed once per cluster: the view-space AABB for the
    // sphere tests, and (derived from it — view->world is rigid, lengths hold)
    // the world-space bounding sphere for the spot cone test.
    float3 froxelMn, froxelMx;
    froxelViewAABB(camera, screenSize, tileMin, tileMax, sliceZ0, sliceZ1, froxelMn, froxelMx);
    float3 froxelCV = 0.5 * (froxelMn + froxelMx);
    float froxelR = length(froxelMx - froxelCV);
    float3 froxelC = (camera.invView * float4(froxelCV, 1.0)).xyz;

    // Strided cooperative tests; hits compact through the shared counters.
    // List order becomes nondeterministic — every consumer is order-independent.
    for (uint i = lid; i < lightCount; i += WG_SIZE) {
        PointLight light = pointLights[i];
        float3 pv = (camera.view * float4(light.position, 1.0)).xyz;
        if (sphereAABBIntersect(pv, light.radius, froxelMn, froxelMx)) {
            uint slot = atomic_fetch_add_explicit(&sPointCount, 1u, memory_order_relaxed);
            if (slot < MAX_LIGHTS_PER_TILE) sPoint[slot] = i;
        }
    }

    // Spot lights: range-sphere test as a cheap pre-reject, then the cone test
    // drops froxels inside the sphere but outside the actual cone (a big win
    // for narrow spots, which fill a small fraction of their range sphere).
    for (uint i = lid; i < spotLightCount; i += WG_SIZE) {
        SpotLight sl = spotLights[i];
        float3 sv = (camera.view * float4(sl.position, 1.0)).xyz;
        if (sphereAABBIntersect(sv, sl.radius, froxelMn, froxelMx) &&
            spotConeIntersectsSphere(sl.position, normalize(sl.direction), sl.radius,
                                     sl.cosOuter, froxelC, froxelR)) {
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
        float3 rv = (camera.view * float4(float3(rl.position), 1.0)).xyz;
        if (sphereAABBIntersect(rv, range, froxelMn, froxelMx)) {
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