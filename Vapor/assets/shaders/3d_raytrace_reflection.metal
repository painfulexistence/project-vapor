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
//   - user_instance_id -> InstanceData -> materialID (the TLAS is built with
//     UserID descriptors for exactly this),
//   - primitive_id + barycentrics fetch the hit triangle from the merged
//     geometry: interpolated vertex normal (Metal exposes no built-in hit
//     normal) taken to world through the instance model matrix, and the
//     interpolated UV that samples the material's ALBEDO texture x base color,
//   - material emissive,
//   - one occlusion ray toward the sun for direct visibility,
//   - environment irradiance (rough envMap mip along the hit normal) as the
//     ambient term.
// GIBS, when enabled, ADDS its surfel radiance as the indirect term — the
// reflection no longer depends on it to show geometry.
//
// The geometry/albedo fetch is gated on params.hasBindlessGeo (merged buffers
// + material table bound); without it the kernel falls back to base color and
// the mirror-ray normal.
//
// Mirror-only (no roughness cone jitter): the output is noise-free and needs NO
// temporal denoise. The PBR composite fades it by roughness.
//
// Output: RGBA16F, rgb = reflected radiance, a = 1 hit / 0 miss.
// ============================================================================

struct ReflectionParams {
    float rayBias;
    float rayMaxDistance;
    uint frameIndex;
    uint hasBindlessGeo;  // 1 = merged geometry + material table bound (fetch UV/normal/albedo)
};

// One entry per material in the bindless argument table (same slot order as
// 3d_pbr_normal_mapped.metal's MaterialTexs — createTextureArgumentTable with
// texturesPerEntry=6). Only `albedo` is sampled here.
struct MaterialTexs {
    texture2d<float, access::sample> albedo    [[id(0)]];
    texture2d<float, access::sample> normal    [[id(1)]];
    texture2d<float, access::sample> metallic  [[id(2)]];
    texture2d<float, access::sample> roughness [[id(3)]];
    texture2d<float, access::sample> occlusion [[id(4)]];
    texture2d<float, access::sample> emissive  [[id(5)]];
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
    // Bound only when params.hasBindlessGeo — merged scene geometry + the
    // per-material texture table, for albedo/UV/normal fetch at the hit.
    device const VertexData*         meshVertices      [[buffer(10)]],
    device const uint*               meshIndices       [[buffer(11)]],
    const device MaterialTexs*       materialTexs      [[buffer(12)]],
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

        uint iid = hit.user_instance_id;
        InstanceData inst = instances[iid];
        float3x3 model33 = float3x3(inst.model[0].xyz, inst.model[1].xyz, inst.model[2].xyz);
        MaterialData mat = materials[inst.materialID];
        float3 base = mat.baseColorFactor.rgb;

        // Hit surface normal. Metal's triangle intersector exposes NO built-in
        // hit normal, so fetch the hit triangle's 3 vertices from the merged
        // geometry (rtIndexOffset + primitive_id*3; indices are mesh-local ->
        // rebased by rtVertexOffset) and interpolate their object-space normals
        // with the barycentrics, then take it to world through the model upper
        // 3x3 (fine for the rigid/uniform transforms the scene uses). When the
        // geometry table isn't bound, fall back to the mirror ray as the normal.
        float3 hitN;
        if (params.hasBindlessGeo != 0) {
            uint b  = inst.rtIndexOffset + hit.primitive_id * 3u;
            uint i0 = meshIndices[b + 0u] + inst.rtVertexOffset;
            uint i1 = meshIndices[b + 1u] + inst.rtVertexOffset;
            uint i2 = meshIndices[b + 2u] + inst.rtVertexOffset;
            VertexData v0 = meshVertices[i0];
            VertexData v1 = meshVertices[i1];
            VertexData v2 = meshVertices[i2];
            // triangle_barycentric_coord is (u,v) for verts 1,2; vert 0 = 1-u-v.
            float2 bc = hit.triangle_barycentric_coord;
            float  w0 = 1.0 - bc.x - bc.y;
            float3 nObj = w0 * float3(v0.normal) + bc.x * float3(v1.normal) + bc.y * float3(v2.normal);
            hitN = normalize(model33 * nObj);
            // Albedo TEXTURE at the interpolated UV, folded into the base color
            // (mirror ray: base mip, no ray differentials — the PBR composite
            // fades the whole reflection by roughness).
            float2 hitUV = w0 * float2(v0.uv) + bc.x * float2(v1.uv) + bc.y * float2(v2.uv);
            constexpr sampler albedoSampler(address::repeat, filter::linear, mip_filter::linear);
            base *= materialTexs[inst.materialID].albedo.sample(albedoSampler, hitUV, level(0.0)).rgb;
        } else {
            hitN = -R;
        }
        if (dot(hitN, R) > 0.0) hitN = -hitN;

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
