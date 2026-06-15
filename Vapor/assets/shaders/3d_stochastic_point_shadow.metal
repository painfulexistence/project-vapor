#include <metal_stdlib>
#include <metal_raytracing>
using namespace metal;
using raytracing::instance_acceleration_structure;
#include "Res/shaders/3d_common.metal"

// Stochastic point light shadow pass (MegaLights-style).
// For each pixel: pick 2 point lights from the tile cluster, cast 1 shadow ray each.
// Output: R16F texture where 0=shadowed, 1=lit (averaged over the 2 samples).
// The caller (PBR shader) uses this as a single shadow multiplier applied to the
// stochastically-chosen lights; temporal accumulation denoises the result.
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
    uint2 tid [[thread_position_in_grid]]
) {
    uint w = shadowTexture.get_width();
    uint h = shadowTexture.get_height();
    if (tid.x >= w || tid.y >= h) return;

    float depth = depthTexture.read(tid).r;
    if (depth >= 1.0 || is_null_instance_acceleration_structure(TLAS)) {
        shadowTexture.write(float4(1.0), tid);
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

    if (lightCount == 0) {
        shadowTexture.write(float4(1.0), tid);
        return;
    }

    // Stochastic: pick 2 lights using per-pixel per-frame hash
    uint seed = tid.x * 1973u + tid.y * 9277u + frameIndex * 26699u;
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

        raytracing::intersector<raytracing::instancing, raytracing::triangle_data> inter;
        inter.assume_geometry_type(raytracing::geometry_type::triangle);
        auto hit = inter.intersect(ray, TLAS, 0xFF);
        visibility += (hit.type == raytracing::intersection_type::none) ? 1.0 : 0.0;
    }

    visibility /= float(sampleCount);
    shadowTexture.write(float4(visibility, 0.0, 0.0, 1.0), tid);
}
