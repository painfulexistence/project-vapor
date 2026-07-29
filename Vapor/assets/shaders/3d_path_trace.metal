#include <metal_stdlib>
#include <metal_raytracing>
using namespace metal;
using raytracing::instance_acceleration_structure;
#include "Res/shaders/3d_common.metal"

// ============================================================================
// Progressive path tracer (photo mode).
//
// Unlike every other RT kernel in the engine, this one does NOT start from the
// raster G-buffer: it generates its own camera rays, so primary visibility,
// shading and occlusion all come from the same light transport. That is the
// point — the raster path approximates, this one converges.
//
// Per pixel, per sample: a camera ray with sub-pixel jitter (that jitter IS the
// antialiasing), then a bounce loop of
//   emissive  ->  next-event estimation toward every directional light
//             ->  BSDF importance sample  ->  Russian roulette,
// against the same TLAS, InstanceData, MaterialData and bindless material
// table the raster and RT-reflection paths use. Nothing scene-side is built
// for photo mode; it re-reads what is already resident.
//
// Radiance sums into an RGBA32F accumulator: rgb = summed radiance, a = sample
// count. pathTraceResolve divides and writes HDR into the scene colour target,
// so the existing bloom/tonemap/post chain finishes the image unchanged.
//
// The RNG stream is keyed on (pixel, absolute sample index) only — never on
// frame number — so N accumulated samples of a fixed camera produce the same
// image every run. That reproducibility is what makes a render comparable
// against a reference.
//
// Light sampling covers every analytic light type, matching the raster
// path's conventions exactly so a converged photo is comparable with a game
// frame of the same scene:
//   - Directional: cone-sampled sun disk (angularRadius 0 = delta).
//   - Point/spot: punctual, 1/d² windowed to zero at `radius` by the same
//     smoothstep the raster shader uses — which makes skipping lights beyond
//     their radius EXACT, not a cutoff approximation. Spot adds the squared
//     cone falloff. One shadow ray each, capped at the light's distance (the
//     light is not geometry; a wall BEHIND it must not occlude it).
//   - Rect: true area sampling, uniform over the quad, double-sided (the
//     raster path's polygon formula is double-sided too). Emitted radiance is
//     color * intensity / PI — the value at which this estimator's expectation
//     EQUALS the raster path's Baum/Arvo solid-angle diffuse term, which is
//     exact. The raster MRP specular is where the two legitimately diverge.
//   No MIS anywhere: none of these lights exist in the TLAS, so a BSDF ray
//   can never hit one — NEE is the only estimator of each, and the estimators
//   partition by construction. Cost: sharp reflections of rect lights carry
//   more variance than a hittable-emitter + MIS design would.
//
// Two known approximations, both deliberate at this stage:
//   - The environment cube is the prefiltered IBL map, not a sharp capture.
//     If the sky's sun disk survives prefiltering it is counted a second time
//     on top of the analytic sun NEE. The disk is smeared across a 128 cube's
//     mip 0 and the analytic sun dominates by orders of magnitude, so the
//     error is small — but it is an error, and a sharp env capture removes it.
//   - Video-textured rect lights emit their flat `color`, not the video
//     frame's average — the video texture is not bound here.
// ============================================================================

struct PathTraceParams {
    uint  sampleOffset;       // samples already in the accumulator; also the RNG stream offset
    uint  samplesPerFrame;
    uint  maxBounces;         // 0 = primary visibility + direct light only
    uint  dirLightCount;
    float sunAngularRadius;   // radians; 0 = hard (delta) sun
    float rayBias;
    float rayMaxDistance;
    float envIntensity;
    float fireflyClamp;       // per-sample radiance ceiling; <= 0 disables
    uint  hasBindlessGeo;     // 1 = merged geometry + material table bound
    uint  resetAccumulation;  // 1 = overwrite the accumulator instead of adding
    uint  pointLightCount;
    uint  spotLightCount;
    uint  rectLightCount;
    uint  _pad0;
    uint  _pad1;
};

// One entry per material in the bindless argument table — same slot order as
// 3d_pbr_normal_mapped.metal's MaterialTexs (createTextureArgumentTable with
// texturesPerEntry=6).
struct MaterialTexs {
    texture2d<float, access::sample> albedo    [[id(0)]];
    texture2d<float, access::sample> normal    [[id(1)]];
    texture2d<float, access::sample> metallic  [[id(2)]];
    texture2d<float, access::sample> roughness [[id(3)]];
    texture2d<float, access::sample> occlusion [[id(4)]];
    texture2d<float, access::sample> emissive  [[id(5)]];
};

// Surface properties at a hit, in the metallic-roughness parameterization the
// raster path uses (3d_pbr_normal_mapped.metal: roughness from .g, metallic
// from .b, albedo and emissive linearized from sRGB).
struct HitSurface {
    float3 position;
    float3 normal;     // interpolated vertex normal, flipped to face the ray
    float3 albedo;     // linear
    float3 emissive;   // linear
    float  metallic;
    float  roughness;
};

// Branchless orthonormal basis around n (Duff et al. 2017).
static void buildBasis(float3 n, thread float3& t, thread float3& b) {
    float sign = n.z >= 0.0 ? 1.0 : -1.0;
    float a = -1.0 / (sign + n.z);
    float c = n.x * n.y * a;
    t = float3(1.0 + sign * n.x * n.x * a, sign * c, -sign * n.x);
    b = float3(c, sign + n.y * n.y * a, -n.y);
}

// Smith height-correlated G1 for GGX (the separable form: G2 = G1(V) * G1(L)).
static float smithG1(float NoX, float alpha) {
    float a2 = alpha * alpha;
    float denom = NoX + sqrt(a2 + (1.0 - a2) * NoX * NoX);
    return denom > 0.0 ? (2.0 * NoX) / denom : 0.0;
}

static float ggxD(float NoH, float alpha) {
    float a2 = alpha * alpha;
    float d = NoH * NoH * (a2 - 1.0) + 1.0;
    return a2 / max(PI * d * d, 1e-9);
}

static float3 fresnelSchlick(float VoH, float3 F0) {
    float f = pow(saturate(1.0 - VoH), 5.0);
    return F0 + (1.0 - F0) * f;
}

// Visible-normal (VNDF) GGX sample, tangent space with N = (0,0,1)
// (Heitz 2018, "Sampling the GGX Distribution of Visible Normals").
static float3 sampleGGXVNDF(float3 Ve, float alpha, float2 u) {
    float3 Vh = normalize(float3(alpha * Ve.x, alpha * Ve.y, Ve.z));
    float lensq = Vh.x * Vh.x + Vh.y * Vh.y;
    float3 T1 = lensq > 0.0 ? float3(-Vh.y, Vh.x, 0.0) * rsqrt(lensq) : float3(1.0, 0.0, 0.0);
    float3 T2 = cross(Vh, T1);
    float r = sqrt(u.x);
    float phi = 2.0 * PI * u.y;
    float t1 = r * cos(phi);
    float t2 = r * sin(phi);
    float s = 0.5 * (1.0 + Vh.z);
    t2 = (1.0 - s) * sqrt(saturate(1.0 - t1 * t1)) + s * t2;
    float3 Nh = t1 * T1 + t2 * T2 + sqrt(saturate(1.0 - t1 * t1 - t2 * t2)) * Vh;
    return normalize(float3(alpha * Nh.x, alpha * Nh.y, max(Nh.z, 0.0)));
}

// Full BSDF value (diffuse Lambert + GGX specular), WITHOUT the N.L factor.
// Used by next-event estimation, where the light direction is given.
static float3 evalBSDF(HitSurface surf, float3 N, float3 V, float3 L) {
    float NoL = dot(N, L);
    float NoV = dot(N, V);
    if (NoL <= 0.0 || NoV <= 0.0) return float3(0.0);

    float3 H = normalize(V + L);
    float NoH = saturate(dot(N, H));
    float VoH = saturate(dot(V, H));

    float3 F0 = mix(float3(0.04), surf.albedo, surf.metallic);
    float3 F = fresnelSchlick(VoH, F0);
    float alpha = max(surf.roughness * surf.roughness, 1e-3);

    float3 spec = F * (ggxD(NoH, alpha) * smithG1(NoV, alpha) * smithG1(NoL, alpha))
                / max(4.0 * NoV * NoL, 1e-6);
    float3 diffuse = surf.albedo * (1.0 - surf.metallic) * (1.0 - F) / PI;
    return diffuse + spec;
}

// Importance-sample the BSDF. Returns false when the sample is degenerate.
// `weight` is already f * cos / pdf, so the caller just multiplies throughput.
static bool sampleBSDF(HitSurface surf, float3 N, float3 V,
                       thread uint& rngState,
                       thread float3& outDir, thread float3& weight) {
    float NoV = dot(N, V);
    if (NoV <= 0.0) return false;

    float3 F0 = mix(float3(0.04), surf.albedo, surf.metallic);
    float3 diffuseAlbedo = surf.albedo * (1.0 - surf.metallic);

    // Lobe selection weighted by each lobe's rough energy, so neither lobe is
    // starved of samples on strongly metallic or strongly diffuse surfaces.
    float diffuseWeight = dot(diffuseAlbedo, float3(1.0)) / 3.0;
    float specWeight = dot(fresnelSchlick(NoV, F0), float3(1.0)) / 3.0;
    float total = diffuseWeight + specWeight;
    float pSpec = total > 0.0 ? saturate(specWeight / total) : 1.0;
    // Never fully starve either lobe: a 0 probability makes the other lobe's
    // 1/p blow up on the first pixel where it is picked.
    pSpec = clamp(pSpec, 0.1, 0.9);

    float3 T, B;
    buildBasis(N, T, B);

    if (randomNext(rngState) < pSpec) {
        // Specular: VNDF half-vector sample, reflected.
        float alpha = max(surf.roughness * surf.roughness, 1e-3);
        float3 Vt = float3(dot(V, T), dot(V, B), dot(V, N));
        float2 u = float2(randomNext(rngState), randomNext(rngState));
        float3 Ht = sampleGGXVNDF(Vt, alpha, u);
        float3 Lt = reflect(-Vt, Ht);
        if (Lt.z <= 0.0) return false;

        outDir = normalize(Lt.x * T + Lt.y * B + Lt.z * N);
        float VoH = saturate(dot(Vt, Ht));
        float3 F = fresnelSchlick(VoH, F0);
        // VNDF weight collapses to F * G2/G1(V); with separable Smith that is
        // F * G1(L). No D, no pdf division — that is the whole point of VNDF.
        weight = F * smithG1(Lt.z, alpha) / pSpec;
        return true;
    }

    // Diffuse: cosine-weighted hemisphere. f * cos / pdf collapses to the
    // albedo, minus the Fresnel share already carried by the specular lobe.
    float2 u = float2(randomNext(rngState), randomNext(rngState));
    outDir = sampleCosineWeightedHemisphere(u, N);
    if (dot(outDir, N) <= 0.0) return false;
    float3 H = normalize(V + outDir);
    float3 F = fresnelSchlick(saturate(dot(V, H)), F0);
    weight = diffuseAlbedo * (1.0 - F) / (1.0 - pSpec);
    return true;
}

// Any-hit occlusion query against the scene. maxDist caps the ray so a
// punctual/area light's shadow ray stops AT the light — the light itself is
// not in the TLAS, and geometry behind it must not occlude it.
static bool shadowRayOccluded(instance_acceleration_structure TLAS,
                              float3 origin, float3 dir, float maxDist) {
    if (maxDist <= 0.001) return false;  // light effectively at the surface
    raytracing::intersector<raytracing::instancing> occ;
    occ.assume_geometry_type(raytracing::geometry_type::triangle);
    occ.accept_any_intersection(true);
    raytracing::ray r;
    r.origin = origin;
    r.direction = dir;
    r.min_distance = 0.001;
    r.max_distance = maxDist;
    return occ.intersect(r, TLAS, 0xFF).type != raytracing::intersection_type::none;
}

// The raster path's punctual falloff (3d_pbr_lib.metal CalculatePointLight):
// inverse square, windowed to exactly zero at the light's radius.
static float punctualAttenuation(float dist, float radius) {
    float atten = 1.0 / max(dist * dist, 1e-6);
    return atten * (1.0 - smoothstep(radius * 0.8, radius, dist));
}

// The raster path's spot cone (3d_pbr_lib.metal CalculateSpotLight): squared
// linear ramp between the outer (zero) and inner (full) half-angle cosines.
// `L` is the surface->light direction; light.direction points FROM the light.
static float spotConeFactor(float3 L, float3 spotDirection, float cosInner, float cosOuter) {
    float cosAngle = dot(-L, spotDirection);
    float cone = clamp((cosAngle - cosOuter) / max(cosInner - cosOuter, 1e-4), 0.0, 1.0);
    return cone * cone;
}

// A direction inside the sun's disk. angularRadius == 0 gives the delta sun
// (identical to 3d_raytrace_shadow.metal's hard-shadow path).
static float3 sampleSunDirection(float3 sunDir, float angularRadius, float2 rnd) {
    if (angularRadius <= 0.0) return sunDir;
    float3 up = abs(sunDir.y) < 0.99 ? float3(0.0, 1.0, 0.0) : float3(1.0, 0.0, 0.0);
    float3 t1 = normalize(cross(up, sunDir));
    float3 t2 = cross(sunDir, t1);
    float r = sqrt(rnd.x) * tan(angularRadius);
    float phi = rnd.y * 2.0 * PI;
    return normalize(sunDir + t1 * (r * cos(phi)) + t2 * (r * sin(phi)));
}

// Resolve the hit triangle into shading inputs. Metal's triangle intersector
// exposes no built-in normal or UV, so both are fetched from the merged scene
// geometry (rtIndexOffset + primitive_id*3, indices rebased by rtVertexOffset)
// and interpolated with the barycentrics — the same fetch
// 3d_raytrace_reflection.metal performs.
static HitSurface resolveHit(float3 rayOrigin, float3 rayDir, float hitDistance,
                             uint instanceIndex, uint primitiveID, float2 barycentric,
                             device const InstanceData* instances,
                             device const MaterialData* materials,
                             device const VertexData* meshVertices,
                             device const uint* meshIndices,
                             const device MaterialTexs* materialTexs,
                             uint hasBindlessGeo) {
    InstanceData inst = instances[instanceIndex];
    MaterialData mat = materials[inst.materialID];

    HitSurface surf;
    surf.position = rayOrigin + rayDir * hitDistance;
    surf.albedo = srgbToLinear(mat.baseColorFactor.rgb);
    surf.emissive = srgbToLinear(float3(mat.emissiveFactor.rgb)) * mat.emissiveStrength;
    surf.metallic = mat.metallicFactor;
    surf.roughness = mat.roughnessFactor;
    surf.normal = -rayDir;

    if (hasBindlessGeo != 0) {
        float3x3 model33 = float3x3(inst.model[0].xyz, inst.model[1].xyz, inst.model[2].xyz);
        uint b = inst.rtIndexOffset + primitiveID * 3u;
        uint i0 = meshIndices[b + 0u] + inst.rtVertexOffset;
        uint i1 = meshIndices[b + 1u] + inst.rtVertexOffset;
        uint i2 = meshIndices[b + 2u] + inst.rtVertexOffset;
        VertexData v0 = meshVertices[i0];
        VertexData v1 = meshVertices[i1];
        VertexData v2 = meshVertices[i2];
        // triangle_barycentric_coord is (u,v) for verts 1,2; vert 0 = 1-u-v.
        float w0 = 1.0 - barycentric.x - barycentric.y;
        float3 nObj = w0 * float3(v0.normal) + barycentric.x * float3(v1.normal)
                    + barycentric.y * float3(v2.normal);
        surf.normal = normalize(model33 * nObj);

        float2 uv = w0 * float2(v0.uv) + barycentric.x * float2(v1.uv)
                  + barycentric.y * float2(v2.uv);
        constexpr sampler texSampler(address::repeat, filter::linear, mip_filter::linear);
        // Base mip: path-traced rays carry no differentials, so there is no
        // footprint to pick a mip from. Texture aliasing is resolved by the
        // sample count instead, which is what accumulation is for.
        //
        // Textures are pulled out of the table one at a time (the idiom
        // 3d_pbr_normal_mapped.metal uses) rather than copying the whole
        // MaterialTexs entry into a local.
        texture2d<float, access::sample> texAlbedo    = materialTexs[inst.materialID].albedo;
        texture2d<float, access::sample> texMetallic  = materialTexs[inst.materialID].metallic;
        texture2d<float, access::sample> texRoughness = materialTexs[inst.materialID].roughness;
        texture2d<float, access::sample> texEmissive  = materialTexs[inst.materialID].emissive;
        surf.albedo = srgbToLinear(texAlbedo.sample(texSampler, uv, level(0.0)).rgb
                                   * mat.baseColorFactor.rgb);
        surf.roughness = texRoughness.sample(texSampler, uv, level(0.0)).g * mat.roughnessFactor;
        surf.metallic = texMetallic.sample(texSampler, uv, level(0.0)).b * mat.metallicFactor;
        surf.emissive = srgbToLinear(texEmissive.sample(texSampler, uv, level(0.0)).rgb
                                     * mat.emissiveFactor.rgb) * mat.emissiveStrength;
    }

    // Face the shading normal against the incoming ray. Interpolated vertex
    // normals can point away on silhouettes and on single-sided geometry hit
    // from behind; shading with those produces black pixels.
    if (dot(surf.normal, rayDir) > 0.0) surf.normal = -surf.normal;
    surf.roughness = clamp(surf.roughness, 0.015, 1.0);
    surf.metallic = saturate(surf.metallic);
    return surf;
}

kernel void pathTraceAccumulate(
    texture2d<float, access::read_write>  accumTexture [[texture(0)]],
    texturecube<float, access::sample>    envMap       [[texture(1)]],
    constant CameraData&                  camera       [[buffer(0)]],
    instance_acceleration_structure       TLAS         [[buffer(1)]],
    constant PathTraceParams&             params       [[buffer(2)]],
    device const InstanceData*            instances    [[buffer(3)]],
    device const MaterialData*            materials    [[buffer(4)]],
    device const DirLight*                dirLights    [[buffer(5)]],
    // Bound only when params.hasBindlessGeo: merged scene geometry + the
    // per-material texture table, for normal/UV/albedo fetch at each hit.
    device const VertexData*              meshVertices [[buffer(6)]],
    device const uint*                    meshIndices  [[buffer(7)]],
    const device MaterialTexs*            materialTexs [[buffer(8)]],
    // Analytic light pools (same buffers the cluster cull and PBR bind); the
    // counts in params gate every read.
    device const PointLight*              pointLights  [[buffer(9)]],
    device const SpotLight*               spotLights   [[buffer(10)]],
    device const RectLight*               rectLights   [[buffer(11)]],
    uint2 tid [[thread_position_in_grid]]
) {
    uint w = accumTexture.get_width();
    uint h = accumTexture.get_height();
    if (tid.x >= w || tid.y >= h) return;

    constexpr sampler envSampler(filter::linear, mip_filter::linear);

    float4 accum = params.resetAccumulation != 0 ? float4(0.0) : accumTexture.read(tid);

    if (is_null_instance_acceleration_structure(TLAS)) {
        accumTexture.write(accum, tid);
        return;
    }

    raytracing::intersector<raytracing::triangle_data, raytracing::instancing> primary;
    primary.assume_geometry_type(raytracing::geometry_type::triangle);

    uint spp = max(params.samplesPerFrame, 1u);
    for (uint s = 0; s < spp; s++) {
        uint sampleIndex = params.sampleOffset + s;
        // Keyed on pixel and absolute sample index only — not on frame number —
        // so a given sample count always reproduces the same image.
        uint rngState = (tid.x * 1973u + tid.y * 9277u) ^ (sampleIndex * 26699u + 1u);
        randomNext(rngState);  // decorrelate neighbours before first use

        // Camera ray through a jittered point in the pixel. The jitter is the
        // antialiasing: with enough samples the pixel integrates its true area.
        float2 jitter = float2(randomNext(rngState), randomNext(rngState));
        float2 uv = (float2(tid) + jitter) / float2(w, h);
        uv.y = 1.0 - uv.y;
        float4 ndc = float4(uv * 2.0 - 1.0, 0.0, 1.0);  // z = 0: near plane (Metal ZO clip)
        float4 viewPos = camera.invProj * ndc;
        viewPos /= viewPos.w;
        float3 nearPoint = (camera.invView * viewPos).xyz;

        float3 rayOrigin = camera.position;
        float3 rayDir = normalize(nearPoint - camera.position);

        float3 radiance = float3(0.0);
        float3 throughput = float3(1.0);

        for (uint bounce = 0; bounce <= params.maxBounces; bounce++) {
            raytracing::ray r;
            r.origin = rayOrigin;
            r.direction = rayDir;
            // Matches the engine's other RT kernels. There is no near-plane
            // clipping in a ray trace: geometry closer than the raster near
            // plane is visible here, which is correct.
            r.min_distance = 0.001;
            r.max_distance = params.rayMaxDistance;

            auto hit = primary.intersect(r, TLAS, 0xFF);
            if (hit.type != raytracing::intersection_type::triangle) {
                radiance += throughput * envMap.sample(envSampler, rayDir, level(0.0)).rgb
                          * params.envIntensity;
                break;
            }

            HitSurface surf = resolveHit(rayOrigin, rayDir, hit.distance,
                                         hit.user_instance_id, hit.primitive_id,
                                         hit.triangle_barycentric_coord,
                                         instances, materials, meshVertices, meshIndices,
                                         materialTexs, params.hasBindlessGeo);

            radiance += throughput * surf.emissive;

            float3 V = -rayDir;
            float3 N = surf.normal;

            // ── Next-event estimation over every analytic light, one shadow
            // ray each. None of these lights exist in the TLAS, so a BSDF ray
            // can never hit one — NEE is each light's only estimator and no
            // MIS weight applies. Falloff/cone/radiance conventions mirror
            // 3d_pbr_lib.metal so the converged image is comparable with the
            // raster frame (see the header).
            const float3 shadowOrigin = surf.position + N * params.rayBias;

            for (uint li = 0; li < params.dirLightCount; li++) {
                DirLight light = dirLights[li];
                float3 sunDir = normalize(-light.direction);
                float2 rnd = float2(randomNext(rngState), randomNext(rngState));
                float3 L = sampleSunDirection(sunDir, params.sunAngularRadius, rnd);

                float NoL = dot(N, L);
                if (NoL <= 0.0) continue;

                float3 f = evalBSDF(surf, N, V, L);
                if (all(f <= float3(0.0))) continue;
                if (shadowRayOccluded(TLAS, shadowOrigin, L, params.rayMaxDistance)) continue;

                radiance += throughput * f * NoL * light.color * light.intensity;
            }

            for (uint li = 0; li < params.pointLightCount; li++) {
                PointLight light = pointLights[li];
                float3 toLight = light.position - surf.position;
                float dist = length(toLight);
                // The window function is exactly zero at radius, so this skip
                // is exact — it is the raster falloff's own boundary.
                if (dist >= light.radius || dist < 1e-4) continue;
                float3 L = toLight / dist;

                float NoL = dot(N, L);
                if (NoL <= 0.0) continue;

                float3 f = evalBSDF(surf, N, V, L);
                if (all(f <= float3(0.0))) continue;
                if (shadowRayOccluded(TLAS, shadowOrigin, L, dist - 0.005)) continue;

                radiance += throughput * f * NoL * light.color * light.intensity
                          * punctualAttenuation(dist, light.radius);
            }

            for (uint li = 0; li < params.spotLightCount; li++) {
                SpotLight light = spotLights[li];
                float3 toLight = light.position - surf.position;
                float dist = length(toLight);
                if (dist >= light.radius || dist < 1e-4) continue;
                float3 L = toLight / dist;

                float cone = spotConeFactor(L, light.direction, light.cosInner, light.cosOuter);
                if (cone <= 0.0) continue;

                float NoL = dot(N, L);
                if (NoL <= 0.0) continue;

                float3 f = evalBSDF(surf, N, V, L);
                if (all(f <= float3(0.0))) continue;
                if (shadowRayOccluded(TLAS, shadowOrigin, L, dist - 0.005)) continue;

                radiance += throughput * f * NoL * light.color * light.intensity
                          * punctualAttenuation(dist, light.radius) * cone;
            }

            for (uint li = 0; li < params.rectLightCount; li++) {
                RectLight light = rectLights[li];
                float3 lp = float3(light.position);
                float3 lr = float3(light.right);
                float3 lu = float3(light.up);

                // Uniform point on the quad; pdf_A = 1/area folds into the
                // area * cosLight / dist² geometry factor below.
                float2 rnd = float2(randomNext(rngState), randomNext(rngState));
                float3 q = lp + lr * (light.halfWidth * (2.0 * rnd.x - 1.0))
                              + lu * (light.halfHeight * (2.0 * rnd.y - 1.0));

                float3 toQ = q - surf.position;
                float distSq = max(dot(toQ, toQ), 1e-6);
                float dist = sqrt(distSq);
                float3 L = toQ / dist;

                float NoL = dot(N, L);
                if (NoL <= 0.0) continue;
                // Double-sided emitter, like the raster polygon formula.
                float cosLight = abs(dot(normalize(cross(lr, lu)), L));
                if (cosLight < 1e-4) continue;

                float3 f = evalBSDF(surf, N, V, L);
                if (all(f <= float3(0.0))) continue;
                if (shadowRayOccluded(TLAS, shadowOrigin, L, dist - 0.005)) continue;

                float area = 4.0 * light.halfWidth * light.halfHeight;
                // color * intensity / PI: the emitted radiance at which this
                // estimator's expectation equals the raster path's exact
                // solid-angle diffuse term (see header). Video-textured lights
                // emit their flat color — the video texture is not bound here.
                float3 emitted = float3(light.color) * (light.intensity / PI);
                radiance += throughput * f * NoL * emitted * (area * cosLight / distSq);
            }

            if (bounce == params.maxBounces) break;

            float3 nextDir, weight;
            if (!sampleBSDF(surf, N, V, rngState, nextDir, weight)) break;
            throughput *= weight;
            if (all(throughput <= float3(0.0))) break;

            // Russian roulette once the path has had a chance to gather light.
            // Survivors are scaled by 1/p, which keeps the estimator unbiased.
            if (bounce >= 2) {
                float p = clamp(max(throughput.r, max(throughput.g, throughput.b)), 0.05, 1.0);
                if (randomNext(rngState) > p) break;
                throughput /= p;
            }

            rayOrigin = surf.position + N * params.rayBias;
            rayDir = nextDir;
        }

        // Firefly clamp: a single bright path (small pdf, large contribution)
        // otherwise leaves a stuck white pixel that hundreds of samples cannot
        // average away. Clamping trades a little energy for a usable image.
        if (params.fireflyClamp > 0.0) {
            radiance = min(radiance, float3(params.fireflyClamp));
        }
        // NaN guard: one NaN sample poisons the accumulator permanently, since
        // every later sample adds to it.
        if (any(isnan(radiance)) || any(isinf(radiance))) continue;

        accum.rgb += radiance;
        accum.a += 1.0;
    }

    accumTexture.write(accum, tid);
}

// Divide the accumulated sum by the sample count and write linear HDR into the
// scene colour target, where the ordinary bloom/tonemap/post chain picks it up.
kernel void pathTraceResolve(
    texture2d<float, access::read>   accumTexture  [[texture(0)]],
    texture2d<float, access::write>  outputTexture [[texture(1)]],
    uint2 tid [[thread_position_in_grid]]
) {
    uint w = outputTexture.get_width();
    uint h = outputTexture.get_height();
    if (tid.x >= w || tid.y >= h) return;

    float4 accum = accumTexture.read(tid);
    float3 color = accum.a > 0.0 ? accum.rgb / accum.a : float3(0.0);
    outputTexture.write(float4(color, 1.0), tid);
}
