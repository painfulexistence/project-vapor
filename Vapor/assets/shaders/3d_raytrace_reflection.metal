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
// ray against the scene TLAS, and shade the hit STANDALONE:
//   - triangle_data intersector tag -> geometric hit normal (object space,
//     taken to world through the instance model matrix),
//   - user_instance_id -> InstanceData -> materialID -> material base color
//     + emissive (the TLAS is built with UserID descriptors for exactly this),
//   - one occlusion ray toward the sun for direct visibility,
//   - environment irradiance (rough envMap mip along the hit normal) as the
//     ambient term.
// GIBS, when enabled, ADDS its surfel radiance as the indirect term — the
// reflection no longer depends on it to show geometry.
//
// Deliberate v1 trade-offs:
//   - Base-color shading only: sampling the hit's albedo TEXTURE needs a
//     UV + vertex fetch, i.e. bindless vertex/index buffers (RHI v2).
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
    device const InstanceData*       instances         [[buffer(7)]],
    device const MaterialData*       materials         [[buffer(8)]],
    device const DirLight*           directionalLights [[buffer(9)]],
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

    // Closest hit (not any-hit): we need the hit DISTANCE to locate the point,
    // triangle_data for the geometric normal, instancing for user_instance_id.
    raytracing::intersector<raytracing::triangle_data, raytracing::instancing> inter;
    inter.assume_geometry_type(raytracing::geometry_type::triangle);
    auto hit = inter.intersect(ray, TLAS, 0xFF);

    float3 color;
    float hitMask;
    if (hit.type == raytracing::intersection_type::triangle) {
        float3 hitPos = ray.origin + R * hit.distance;

        // Geometric hit normal: object space from the intersector, world via
        // the instance's model matrix (upper 3x3 — fine for the rigid/uniform
        // transforms the scene uses; a full inverse-transpose is not worth the
        // per-ray cost here). Flip toward the incoming ray: the geometric
        // normal has no guaranteed winding.
        uint iid = hit.user_instance_id;
        InstanceData inst = instances[iid];
        float3x3 model33 = float3x3(inst.model[0].xyz, inst.model[1].xyz, inst.model[2].xyz);
        float3 hitN = normalize(model33 * hit.triangle_normal);
        if (dot(hitN, R) > 0.0) hitN = -hitN;

        MaterialData mat = materials[inst.materialID];
        float3 base = mat.baseColorFactor.rgb;

        // Direct sun: one occlusion ray (matches the RT shadow kernel's setup).
        DirLight sun = directionalLights[0];
        float3 L = normalize(-sun.direction);
        float sunVis = 0.0;
        float nl = max(dot(hitN, L), 0.0);
        if (nl > 0.0) {
            raytracing::ray shadowRay;
            shadowRay.origin = hitPos + hitN * params.rayBias;
            shadowRay.direction = L;
            shadowRay.min_distance = 0.001;
            shadowRay.max_distance = params.rayMaxDistance;
            raytracing::intersector<raytracing::instancing> occ;
            occ.assume_geometry_type(raytracing::geometry_type::triangle);
            occ.accept_any_intersection(true);
            auto sh = occ.intersect(shadowRay, TLAS, 0xFF);
            sunVis = (sh.type == raytracing::intersection_type::none) ? 1.0 : 0.0;
        }

        // Ambient: environment irradiance approximated by a rough envMap mip
        // along the hit normal (the same prefiltered cube the miss path uses).
        constexpr sampler envSampler(filter::linear, mip_filter::linear);
        float3 ambient = envMap.sample(envSampler, hitN, level(4.0)).rgb;

        color = base * (sun.color * sun.intensity * (sunVis * nl) + ambient)
              + float3(mat.emissiveFactor) * mat.emissiveStrength;
        // GIBS indirect, when the surfel cache is live (totalCells==0 when off).
        color += base * surfelRadianceAt(hitPos, hitN, surfels, cellHeads, surfelNext, gibs);
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
