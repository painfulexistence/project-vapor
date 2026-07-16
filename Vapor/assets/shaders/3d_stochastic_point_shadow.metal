#include <metal_stdlib>
#include <metal_raytracing>
using namespace metal;
using raytracing::instance_acceleration_structure;
#include "Res/shaders/3d_common.metal"

// Stochastic light shadow pass (MegaLights-style), covering EVERY analytic
// light type. Per pixel:
//   R = point-light visibility (2 lights picked from the tile cluster)
//   G = rect AREA light visibility — a random point is sampled on the quad
//       each frame, so temporal accumulation converges to a true soft penumbra
//   B = spot-light visibility (1 ray to the picked spot's position)
// Channels for absent light types stay 1.0 (fully lit). Legacy R16F targets
// (native path) simply drop G/B; its PBR binds shadowFlags=0 and never reads
// them. Temporal accumulation denoises all three channels together.
kernel void computeMain(
    texture2d<float>              depthTexture    [[texture(0)]],
    texture2d<float>              normalTexture   [[texture(1)]],
    texture2d<float, access::write> shadowTexture [[texture(2)]],
    constant CameraData&          camera          [[buffer(0)]],
    const device PointLight*      pointLights     [[buffer(1)]],
    const device Cluster*         clusters        [[buffer(2)]],
    constant float2&              screenSize      [[buffer(3)]],
    constant uint3&               gridDims        [[buffer(4)]],
    constant uint&                frameIndex      [[buffer(5)]],
    instance_acceleration_structure TLAS          [[buffer(6)]],
    constant uint&                debugMode       [[buffer(7)]], // 0=visibility, 1=tile light-count heatmap
    const device SpotLight*       spotLights      [[buffer(8)]],
    const device RectLight*       rectLights      [[buffer(9)]],
    constant uint2&               extraCounts     [[buffer(10)]], // x=rectCount, y=spotCount
    uint2 tid [[thread_position_in_grid]]
) {
    uint w = shadowTexture.get_width();
    uint h = shadowTexture.get_height();
    if (tid.x >= w || tid.y >= h) return;

    float depth = depthTexture.read(tid).r;
    if (depth >= 1.0 || is_null_instance_acceleration_structure(TLAS)) {
        shadowTexture.write(float4(debugMode == 1 ? 0.0 : 1.0), tid);
        return;
    }

    // Reconstruct world position
    float2 uv = float2(tid) / float2(w, h);
    float2 ndcXY = float2(uv.x, 1.0 - uv.y) * 2.0 - 1.0;
    float4 ndc = float4(ndcXY, depth, 1.0);
    float4 viewPos = camera.invProj * ndc;
    viewPos /= viewPos.w;
    float3 worldPos = (camera.invView * viewPos).xyz;
    float3 worldNormal = normalize(normalTexture.read(tid).xyz);

    // Find tile cluster — match PBR shader convention exactly (2D tile, Y flipped)
    uint gridX = gridDims.x;
    uint gridY = gridDims.y;
    uint tileX = uint(uv.x * float(gridX));
    uint tileY = uint((1.0 - uv.y) * float(gridY));

    uint clusterIdx = tileX + tileY * gridX;
    const device Cluster& cluster = clusters[clusterIdx];
    uint lightCount = cluster.lightCount;

    // Debug: visualize per-tile light count (0 lights = black, 8+ = white)
    if (debugMode == 1) {
        shadowTexture.write(float4(float(lightCount) / 8.0), tid);
        return;
    }

    uint seed = tid.x * 1973u + tid.y * 9277u + frameIndex * 26699u;
    raytracing::intersector<raytracing::instancing> occl;
    occl.assume_geometry_type(raytracing::geometry_type::triangle);
    occl.accept_any_intersection(true);

    // --- R: point lights (2 picks from the tile cluster) -------------------
    float pointVis = 1.0;
    if (lightCount > 0) {
        float2 r0 = random(seed);
        float2 r1 = random(seed + 1u);
        uint lightA = cluster.lightIndices[uint(r0.x * float(lightCount)) % lightCount];
        uint lightB = cluster.lightIndices[uint(r1.x * float(lightCount)) % lightCount];

        float visibility = 0.0;
        uint sampleCount = (lightA == lightB) ? 1u : 2u;
        for (uint s = 0; s < sampleCount; s++) {
            uint li = (s == 0) ? lightA : lightB;
            PointLight light = pointLights[li];

            float3 toLight = light.position - worldPos;
            float dist = length(toLight);
            if (dist >= light.radius) { visibility += 1.0; continue; }

            float3 dir = toLight / dist;
            // Skip lights behind the surface
            if (dot(dir, worldNormal) <= 0.0) { visibility += 1.0; continue; }

            raytracing::ray ray;
            ray.origin = worldPos + worldNormal * 0.005;
            ray.direction = dir;
            ray.min_distance = 0.001;
            ray.max_distance = dist - 0.01;
            auto hit = occl.intersect(ray, TLAS, 0xFF);
            visibility += (hit.type == raytracing::intersection_type::none) ? 1.0 : 0.0;
        }
        pointVis = visibility / float(sampleCount);
    }

    // --- G: rect AREA lights (random point on the quad -> soft penumbra) ---
    float rectVis = 1.0;
    if (extraCounts.x > 0) {
        uint ri = uint(random(seed + 2u).x * float(extraCounts.x)) % extraCounts.x;
        RectLight rl = rectLights[ri];
        float2 rq = random(seed + 3u) * 2.0 - 1.0;   // uniform on the quad
        float3 target = float3(rl.position) + float3(rl.right) * (rq.x * rl.halfWidth)
                                            + float3(rl.up) * (rq.y * rl.halfHeight);
        float3 toLight = target - worldPos;
        float dist = length(toLight);
        if (dist > 0.01) {
            float3 dir = toLight / dist;
            if (dot(dir, worldNormal) > 0.0) {
                raytracing::ray ray;
                ray.origin = worldPos + worldNormal * 0.005;
                ray.direction = dir;
                ray.min_distance = 0.001;
                ray.max_distance = dist - 0.01;
                auto hit = occl.intersect(ray, TLAS, 0xFF);
                rectVis = (hit.type == raytracing::intersection_type::none) ? 1.0 : 0.0;
            }
        }
    }

    // --- B: spot lights (1 ray to the picked spot's apex) ------------------
    float spotVis = 1.0;
    if (extraCounts.y > 0) {
        uint si = uint(random(seed + 4u).x * float(extraCounts.y)) % extraCounts.y;
        SpotLight sl = spotLights[si];
        float3 toLight = sl.position - worldPos;
        float dist = length(toLight);
        if (dist < sl.radius && dist > 0.01) {
            float3 dir = toLight / dist;
            // Only shade pixels the cone can reach and that face the light.
            bool inCone = dot(-dir, sl.direction) > sl.cosOuter;
            if (inCone && dot(dir, worldNormal) > 0.0) {
                raytracing::ray ray;
                ray.origin = worldPos + worldNormal * 0.005;
                ray.direction = dir;
                ray.min_distance = 0.001;
                ray.max_distance = dist - 0.01;
                auto hit = occl.intersect(ray, TLAS, 0xFF);
                spotVis = (hit.type == raytracing::intersection_type::none) ? 1.0 : 0.0;
            }
        }
    }

    shadowTexture.write(float4(pointVis, rectVis, spotVis, 1.0), tid);
}
