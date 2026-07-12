#include <metal_stdlib>
#include <metal_raytracing>
using namespace metal;
using raytracing::instance_acceleration_structure;
#include "Res/shaders/3d_common.metal"
#include "Res/shaders/gibs_common.metal"

// ============================================================================
// RT mirror reflections.
//
// Per pixel: reconstruct the surface from depth+normal, trace one reflection
// ray against the scene TLAS, and shade the hit from the GIBS surfel radiance
// cache (irradiance + directLight — the same convention gibs_raytracing.metal
// composites as GI). This avoids per-triangle material fetch entirely: the
// intersector gives us the hit POSITION (origin + t*dir), and the surfel cache
// gives us "what does the scene look like at that point".
//
// Consequences of that design (deliberate v1 trade-offs):
//   - With GIBS enabled the reflection shows the lit scene (best showcase).
//   - With GIBS disabled (activeSurfelCount==0 / totalCells==0) hits shade to
//     black and only ray MISSES contribute (environment map) — the panel says
//     "enable GI for full reflections".
//   - Mirror-only (no roughness cone jitter), so the output is noise-free and
//     needs NO temporal denoise. The PBR composite fades it by roughness.
//
// Output: RGBA16F, rgb = reflected radiance, a = 1 hit / 0 miss.
// ============================================================================

struct ReflectionParams {
    float rayBias;
    float rayMaxDistance;
    uint frameIndex;
    uint _pad;
};

// Nearest-surfel radiance lookup at an arbitrary world position (a trimmed
// copy of gibs_raytracing.metal's sampleSurfelAtPosition — kept local so this
// shader has no dependency on that file's internals).
static float3 surfelRadianceAt(float3 pos, float3 normalApprox,
                               device const Surfel* surfels,
                               device const uint* cellHeads,
                               device const uint* surfelNext,
                               constant GIBSData& gibs) {
    if (gibs.totalCells == 0 || !isInWorldBounds(pos, gibs)) {
        return float3(0.0);
    }
    uint cellHash = computeCellHash(pos, gibs);
    if (cellHash >= gibs.totalCells) {
        return float3(0.0);
    }
    float bestWeight = 0.0;
    float3 best = float3(0.0);
    uint iter = 0;
    for (uint j = cellHeads[cellHash];
         j != GIBS_INVALID_INDEX && iter < GIBS_MAX_CHAIN_LENGTH;
         j = surfelNext[j], iter++) {
        Surfel s = surfels[j];
        if (!(s.flags & SURFEL_FLAG_VALID)) continue;
        float w = computeSurfelWeight(pos, normalApprox, s, gibs.cellSize * 2.0);
        if (w > bestWeight) {
            bestWeight = w;
            best = float3(s.irradiance) + float3(s.directLight);
        }
    }
    return best;
}

kernel void computeMain(
    texture2d<float>                 depthTexture      [[texture(0)]],
    texture2d<float>                 normalTexture     [[texture(1)]],
    texture2d<float, access::write>  reflectionTexture [[texture(2)]],
    texturecube<float, access::sample> envMap          [[texture(3)]],
    constant CameraData&             camera            [[buffer(0)]],
    device const Surfel*             surfels           [[buffer(1)]],
    device const uint*               cellHeads         [[buffer(2)]],
    device const uint*               surfelNext        [[buffer(3)]],
    constant GIBSData&               gibs              [[buffer(4)]],
    instance_acceleration_structure  TLAS              [[buffer(5)]],
    constant ReflectionParams&       params            [[buffer(6)]],
    uint2 tid [[thread_position_in_grid]]
) {
    uint w = reflectionTexture.get_width();
    uint h = reflectionTexture.get_height();
    if (tid.x >= w || tid.y >= h) return;

    // Resolution-agnostic full-res G-buffer read (same trick as the RT shadow
    // kernel: the reflection target may be half-res).
    uint2 fullDim = uint2(depthTexture.get_width(), depthTexture.get_height());
    uint2 scale = max(fullDim / uint2(w, h), 1u);
    uint2 fullTid = min(tid * scale, fullDim - 1);

    float depth = depthTexture.read(fullTid).r;
    if (depth >= 1.0 || is_null_instance_acceleration_structure(TLAS)) {
        reflectionTexture.write(float4(0.0), tid);  // sky / no TLAS: no reflection
        return;
    }

    // Reconstruct world position + normal (RT shadow kernel convention).
    float2 uv = (float2(tid) + 0.5) / float2(w, h);
    uv.y = 1.0 - uv.y;
    float4 ndc = float4(uv * 2.0 - 1.0, depth, 1.0);
    float4 viewPos = camera.invProj * ndc;
    viewPos /= viewPos.w;
    float3 worldPos = (camera.invView * viewPos).xyz;
    float3 N = normalize(normalTexture.read(fullTid).xyz);

    float3 V = normalize(worldPos - camera.position);  // camera -> surface
    float3 R = reflect(V, N);

    raytracing::ray ray;
    ray.origin = worldPos + N * params.rayBias;
    ray.direction = R;
    ray.min_distance = 0.001;
    ray.max_distance = params.rayMaxDistance;

    // Closest hit (not any-hit): we need the hit DISTANCE to locate the point.
    raytracing::intersector<raytracing::instancing> inter;
    inter.assume_geometry_type(raytracing::geometry_type::triangle);
    auto hit = inter.intersect(ray, TLAS, 0xFF);

    float3 color;
    float hitMask;
    if (hit.type == raytracing::intersection_type::triangle) {
        float3 hitPos = ray.origin + R * hit.distance;
        // We have no hit normal without vertex fetch; a surface hit head-on by
        // the ray faces roughly -R, which is exactly what the surfel weight's
        // normal-similarity term wants as an approximation.
        color = surfelRadianceAt(hitPos, -R, surfels, cellHeads, surfelNext, gibs);
        hitMask = 1.0;
    } else {
        // Miss: environment along the reflected direction (sharp mip — this is
        // a mirror ray; the composite handles roughness fade).
        constexpr sampler envSampler(filter::linear, mip_filter::linear);
        color = envMap.sample(envSampler, R, level(0.0)).rgb;
        hitMask = 0.0;
    }
    reflectionTexture.write(float4(color, hitMask), tid);
}
