#include <metal_stdlib>
#include <metal_raytracing>
using namespace metal;
using raytracing::instance_acceleration_structure;
#include "Res/shaders/3d_common.metal"
#include "Res/shaders/gibs_common.metal"

// ============================================================================
// RT refractions (KHR_materials_transmission rendering support).
//
// Structural clone of 3d_raytrace_reflection.metal with the ray direction
// swapped for a refracted one: per pixel, reconstruct the surface from
// depth+normal, refract the view ray through the surface (fixed IOR 1.5 — the
// glTF default; KHR_materials_ior can feed this later), trace it against the
// scene TLAS, and shade the hit from the GIBS surfel radiance cache. Misses
// sample the environment map along the refracted direction.
//
// Deliberate v1 trade-offs (mirroring the reflection pass):
//   - THIN-WALLED: one refraction event at the front surface, no exit event
//     and no absorption along the interior path — glTF transmission's own
//     default model. Total internal reflection falls back to a reflect ray.
//   - Sharp only (no roughness cone), so the output is noise-free and needs
//     no temporal denoise; the PBR composite fades it by roughness.
//   - With GIBS disabled, TLAS hits shade to black and only misses (envmap)
//     contribute — same "enable GI for full quality" caveat as reflections.
//
// The pass runs only when the scene has a material with transmission > 0
// (the PBR composite weights it per pixel by the material's transmission).
// Output: RGBA16F, rgb = transmitted radiance, a = 1 hit / 0 miss.
// ============================================================================

struct RefractionParams {
    float rayBias;
    float rayMaxDistance;
    uint frameIndex;
    uint _pad;
};

// Nearest-surfel radiance lookup (same trimmed copy as the reflection kernel).
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
    texture2d<float, access::write>  refractionTexture [[texture(2)]],
    texturecube<float, access::sample> envMap          [[texture(3)]],
    constant CameraData&             camera            [[buffer(0)]],
    device const Surfel*             surfels           [[buffer(1)]],
    device const uint*               cellHeads         [[buffer(2)]],
    device const uint*               surfelNext        [[buffer(3)]],
    constant GIBSData&               gibs              [[buffer(4)]],
    instance_acceleration_structure  TLAS              [[buffer(5)]],
    constant RefractionParams&       params            [[buffer(6)]],
    device const InstanceData*       instances         [[buffer(7)]],
    device const MaterialData*       materials         [[buffer(8)]],
    device const DirLight*           directionalLights [[buffer(9)]],
    uint2 tid [[thread_position_in_grid]]
) {
    uint w = refractionTexture.get_width();
    uint h = refractionTexture.get_height();
    if (tid.x >= w || tid.y >= h) return;

    uint2 fullDim = uint2(depthTexture.get_width(), depthTexture.get_height());
    uint2 scale = max(fullDim / uint2(w, h), 1u);
    uint2 fullTid = min(tid * scale, fullDim - 1);

    float depth = depthTexture.read(fullTid).r;
    if (depth >= 1.0 || is_null_instance_acceleration_structure(TLAS)) {
        refractionTexture.write(float4(0.0), tid);  // sky / no TLAS
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

    // Air -> glass, IOR 1.5 (glTF default; eta < 1 never total-internal-
    // reflects, the fallback is pure robustness for degenerate normals).
    const float kEta = 1.0 / 1.5;
    float3 T = refract(V, N, kEta);
    bool refracted = dot(T, T) > 1e-6;
    if (!refracted) {
        T = reflect(V, N);
    }
    T = normalize(T);

    raytracing::ray ray;
    // Refracted rays enter the surface: bias AGAINST the normal so the ray
    // starts inside and doesn't re-hit the front face at t~0. The TIR/reflect
    // fallback exits on the camera side and biases along the normal instead.
    ray.origin = worldPos + (refracted ? -N : N) * params.rayBias;
    ray.direction = T;
    ray.min_distance = 0.001;
    ray.max_distance = params.rayMaxDistance;

    // Closest hit (not any-hit): we need the hit DISTANCE to locate the point.
    raytracing::intersector<raytracing::triangle_data, raytracing::instancing> inter;
    inter.assume_geometry_type(raytracing::geometry_type::triangle);
    auto hit = inter.intersect(ray, TLAS, 0xFF);

    float3 color;
    float hitMask;
    if (hit.type == raytracing::intersection_type::triangle) {
        float3 hitPos = ray.origin + T * hit.distance;

        // Standalone hit shading — same recipe as the reflection kernel:
        // geometric normal + material base color + one sun occlusion ray +
        // env-irradiance ambient; GIBS adds indirect when live.
        uint iid = hit.user_instance_id;
        InstanceData inst = instances[iid];
        float3x3 model33 = float3x3(inst.model[0].xyz, inst.model[1].xyz, inst.model[2].xyz);
        float3 hitN = normalize(model33 * hit.triangle_normal);
        if (dot(hitN, T) > 0.0) hitN = -hitN;

        MaterialData mat = materials[inst.materialID];
        float3 base = mat.baseColorFactor.rgb;

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

        constexpr sampler envSampler(filter::linear, mip_filter::linear);
        float3 ambient = envMap.sample(envSampler, hitN, level(4.0)).rgb;

        color = base * (sun.color * sun.intensity * (sunVis * nl) + ambient)
              + float3(mat.emissiveFactor) * mat.emissiveStrength;
        color += base * surfelRadianceAt(hitPos, hitN, surfels, cellHeads, surfelNext, gibs);
        hitMask = 1.0;
    } else {
        constexpr sampler envSampler(filter::linear, mip_filter::linear);
        color = envMap.sample(envSampler, T, level(0.0)).rgb;
        hitMask = 0.0;
    }
    refractionTexture.write(float4(color, hitMask), tid);
}
