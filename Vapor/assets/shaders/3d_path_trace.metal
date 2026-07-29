#include <metal_stdlib>
#include <metal_raytracing>
using namespace metal;
using raytracing::instance_acceleration_structure;
#include "Res/shaders/3d_common.metal"
#include "Res/shaders/3d_pbr_lib.metal"

// ============================================================================
// Progressive path tracer (photo mode) — ground truth for THIS engine's PBR.
//
// Unlike every other RT kernel in the engine, this one does NOT start from the
// raster G-buffer: it generates its own camera rays, so primary visibility,
// shading and occlusion all come from the same light transport.
//
// The BRDF is the raster path's Disney model, not an approximation of it:
// evalDisneyBRDF below is CookTorranceBRDF (3d_pbr_lib.metal) minus its
// trailing N.L factor, assembled from the SAME GTR/Smith/Fresnel primitives —
// this file includes 3d_pbr_lib.metal precisely so the two paths cannot
// drift. Retro-reflective diffuse, subsurface, specular/specularTint-driven
// F0, anisotropy, sheen and clearcoat all behave exactly as they do in the
// raster frame; hit points are normal-mapped through the same TBN
// construction (Gram-Schmidt against the interpolated normal, tangent-w
// bitangent sign, geometric fallback for degenerate tangents, and — like the
// raster shader — no normalScale application). Where the raster model does
// something non-physical (clearcoat and sheen add energy on top), the photo
// reproduces the model, not the physics: this is the engine's reference
// image, not a different renderer's.
//
// Importance sampling is an explicit three-lobe mixture — cosine diffuse,
// anisotropic GGX via visible-normal (VNDF) sampling, and GTR1 clearcoat —
// weighted per surface, with the full mixture pdf evaluated for the sampled
// direction (weight = f * cos / pdf). The general form costs a little more
// than collapsed per-lobe weights but makes the estimator testable: the CPU
// mirror of this math statistically verifies E[weight] equals the numerically
// integrated reflectance, and that evalDisneyBRDF * N.L equals the raster
// CookTorranceBRDF bit for bit.
//
// MASK (alpha-cutout) materials are honored on every ray: primary, bounce and
// shadow rays re-test the albedo alpha at each hit against the material's
// cutoff and pass through failed hits, so foliage renders and shadows as
// leaves, not as solid quads. KHR_materials_transmission is a stochastic thin
// -glass lobe matching the raster refraction pass's convention (IOR 1.5,
// single interface, TIR falls back to reflect, tinted by base color).
//
// Light sampling covers every analytic light type, matching the raster
// conventions (see the NEE section): cone-sampled sun; punctual point/spot
// with the raster falloff window (zero at radius — skipping beyond it is
// exact); double-sided area-sampled rect whose emitted radiance
// color*intensity/PI makes the estimator agree with the raster's exact
// Baum/Arvo diffuse term. No MIS: none of these lights are in the TLAS, so
// NEE is each light's only estimator.
//
// Radiance sums into an RGBA32F accumulator (rgb = sum, a = sample count);
// pathTraceResolve divides and writes HDR into the scene colour target. The
// RNG is keyed on (pixel, absolute sample index) — never the frame number —
// so N samples of a fixed camera reproduce the same image every run.
//
// Known deviations from the raster frame, all deliberate:
//   - The occlusion (AO) texture is NOT applied: baked AO approximates the
//     occlusion this integrator computes for real, and applying both would
//     double-darken. The raster frame uses it; a photo does not need to.
//   - The environment cube is the prefiltered IBL map, not a sharp capture,
//     so the sky is soft and a sun disk surviving prefiltering is counted a
//     second time on top of the analytic sun NEE (small: the analytic sun
//     dominates by orders of magnitude).
//   - Video-textured rect lights emit their flat color (no video texture
//     bound here); prototype-UV (triplanar) materials resolve with their mesh
//     UVs instead.
//   - Shadow rays treat transmissive surfaces as opaque — matching the raster
//     RT shadow pass, which does the same.
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
    uint  hasAlphaMask;       // 1 = some scene material is MASK (alpha-test hits)
    uint  _pad0;
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

// Everything shading needs at a hit: the raster PBR Surface (3d_pbr_lib) plus
// the shading frame and the transmission factor the Surface doesn't carry.
struct HitPoint {
    float3 position;
    float3 N;            // shading normal (normal-mapped when geometry is bound)
    float3 T;            // mesh tangent frame — anisotropy follows UVs, like raster
    float3 B;
    Surface surf;
    float transmission;
};

// Branchless orthonormal basis around n (Duff et al. 2017). Fallback frame
// when the mesh has no usable tangent — mirrors the raster shader's fallback.
static void buildBasis(float3 n, thread float3& t, thread float3& b) {
    float sign = n.z >= 0.0 ? 1.0 : -1.0;
    float a = -1.0 / (sign + n.z);
    float c = n.x * n.y * a;
    t = float3(1.0 + sign * n.x * n.x * a, sign * c, -sign * n.x);
    b = float3(c, sign + n.y * n.y * a, -n.y);
}

// ── Disney BRDF: evaluation and sampling ────────────────────────────────────

// CookTorranceBRDF (3d_pbr_lib.metal) minus its trailing `* nl`, built from
// the same primitives. KEEP line-for-line in sync with that function — the
// CPU test suite asserts evalDisneyBRDF * nl == CookTorranceBRDF on random
// inputs, so drift fails the build's test run, not a render review.
static float3 evalDisneyBRDF(Surface surf, float3 N, float3 T, float3 B, float3 V, float3 L) {
    float nl = dot(N, L);
    float nv = dot(N, V);
    if (nl <= 0.0 || nv <= 0.0) return float3(0.0);

    float3 H = normalize(L + V);
    float nh = max(dot(N, H), 0.0);
    float lh = max(dot(L, H), 0.0);

    float lum = luminance(surf.color);
    float3 tint = lum > 0.0 ? surf.color / lum : float3(1.0);
    float3 spec0 = mix(surf.specular * 0.08 * mix(float3(1.0), tint, surf.specular_tint),
                       surf.color, surf.metallic);
    float fh = FresnelApprox(lh);
    float fl = FresnelApprox(nl);
    float fv = FresnelApprox(nv);
    float fss90 = lh * lh * surf.roughness;
    // diffuse (Disney retro-reflective weight — NOT plain Lambert)
    float fd90 = 0.5 + 2.0 * fss90;
    float kd = mix(1.0, fd90, fl) * mix(1.0, fd90, fv);
    // subsurface
    float fss = mix(1.0, fss90, fl) * mix(1.0, fss90, fv);
    float ss = 1.25 * (fss * (1.0 / (nl + nv + 0.0001) - 0.5) + 0.5);
    // specular (anisotropic GTR2; the Smith form is G1/(2u), so no /4nlnv here)
    float aspect = sqrt(1.0 - surf.anisotropic * .9);
    float ax = max(.001, surf.roughness * surf.roughness / aspect);
    float ay = max(.001, surf.roughness * surf.roughness * aspect);
    float hx = dot(H, T);
    float hy = dot(H, B);
    float D = GTR2_aniso(nh, hx, hy, ax, ay);
    float G = SmithGGX_aniso(nl, dot(L, T), dot(L, B), ax, ay)
            * SmithGGX_aniso(nv, dot(V, T), dot(V, B), ax, ay);
    float3 F = mix(spec0, float3(1.0), fh);
    float3 specular = D * G * F;
    // sheen
    float3 sheen = fh * surf.sheen * mix(float3(1.0), tint, surf.sheen_tint);
    // clearcoat
    float Dr = GTR1(nh, mix(.1, .001, surf.clearcoat_gloss));
    float Fr = mix(.04, 1.0, fh);
    float Gr = SmithGGX(nl, .25) * SmithGGX(nv, .25);
    float3 clearcoat = 0.25 * float3(surf.clearcoat) * Dr * Fr * Gr;

    return (mix(kd, ss, surf.subsurface) * surf.color / PI + sheen) * (1.0 - surf.metallic)
         + specular + clearcoat;
}

// Mixture weights for the three sampled lobes. Correctness does not depend on
// these (the mixture pdf is evaluated in full below); they only steer variance.
// The clamp keeps every present lobe reachable.
struct LobeWeights {
    float pDiffuse;
    float pSpec;
    float pClearcoat;
};

static LobeWeights disneyLobeWeights(Surface surf) {
    float lum = luminance(surf.color);
    float3 tint = lum > 0.0 ? surf.color / lum : float3(1.0);
    float3 spec0 = mix(surf.specular * 0.08 * mix(float3(1.0), tint, surf.specular_tint),
                       surf.color, surf.metallic);
    float wD = (1.0 - surf.metallic) * max(lum, 0.05);
    float wS = max(luminance(spec0), 0.05);
    float wC = 0.25 * surf.clearcoat;
    float total = wD + wS + wC;
    LobeWeights w;
    w.pDiffuse = wD / total;
    w.pSpec = wS / total;
    w.pClearcoat = wC / total;
    return w;
}

// Anisotropic visible-normal GGX sample, tangent space with N = (0,0,1)
// (Heitz 2018, "Sampling the GGX Distribution of Visible Normals").
static float3 sampleGGXVNDFAniso(float3 Ve, float ax, float ay, float2 u) {
    float3 Vh = normalize(float3(ax * Ve.x, ay * Ve.y, Ve.z));
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
    return normalize(float3(ax * Nh.x, ay * Nh.y, max(Nh.z, 0.0)));
}

// Mixture pdf of sampleDisney for direction L. Evaluated in full — this is
// what makes weight = f*cos/pdf an unbiased estimator of the whole BRDF, and
// what the CPU consistency test verifies.
static float disneyPdf(Surface surf, LobeWeights w, float3 N, float3 T, float3 B,
                       float3 V, float3 L) {
    float nl = dot(N, L);
    float nv = dot(N, V);
    if (nl <= 0.0 || nv <= 0.0) return 0.0;
    float3 H = normalize(V + L);
    float nh = max(dot(N, H), 0.0);
    float vh = max(dot(V, H), 1e-6);

    float aspect = sqrt(1.0 - surf.anisotropic * .9);
    float ax = max(.001, surf.roughness * surf.roughness / aspect);
    float ay = max(.001, surf.roughness * surf.roughness * aspect);

    float pdfDiffuse = nl / PI;
    // VNDF pdf: G1(V)·D/(4·nv); with SmithGGX_aniso = G1/(2u) it collapses to
    // SmithGGX_aniso(V)·D/2.
    float D = GTR2_aniso(nh, dot(H, T), dot(H, B), ax, ay);
    float pdfSpec = D * SmithGGX_aniso(nv, dot(V, T), dot(V, B), ax, ay) * 0.5;
    // GTR1 half-vector pdf is Dr·nh (GTR1 is normalized over the hemisphere);
    // the /4vh is the half-vector -> L Jacobian.
    float aCC = mix(.1, .001, surf.clearcoat_gloss);
    float pdfClearcoat = GTR1(nh, aCC) * nh / (4.0 * vh);

    return w.pDiffuse * pdfDiffuse + w.pSpec * pdfSpec + w.pClearcoat * pdfClearcoat;
}

// Importance-sample the Disney BRDF. `weight` = f * cos / pdf(mixture).
static bool sampleDisney(Surface surf, float3 N, float3 T, float3 B, float3 V,
                         thread uint& rngState,
                         thread float3& outDir, thread float3& outWeight) {
    if (dot(N, V) <= 0.0) return false;
    LobeWeights w = disneyLobeWeights(surf);

    float pick = randomNext(rngState);
    float2 u = float2(randomNext(rngState), randomNext(rngState));

    float aspect = sqrt(1.0 - surf.anisotropic * .9);
    float ax = max(.001, surf.roughness * surf.roughness / aspect);
    float ay = max(.001, surf.roughness * surf.roughness * aspect);

    float3 L;
    if (pick < w.pDiffuse) {
        L = sampleCosineWeightedHemisphere(u, N);
    } else if (pick < w.pDiffuse + w.pSpec) {
        float3 Vt = float3(dot(V, T), dot(V, B), dot(V, N));
        float3 Ht = sampleGGXVNDFAniso(Vt, ax, ay, u);
        float3 H = Ht.x * T + Ht.y * B + Ht.z * N;
        L = reflect(-V, H);
    } else {
        // GTR1 half-vector sample (standard Disney clearcoat inversion).
        float a = mix(.1, .001, surf.clearcoat_gloss);
        float a2 = a * a;
        float cosT2 = (1.0 - pow(a2, 1.0 - u.x)) / (1.0 - a2);
        float cosT = sqrt(saturate(cosT2));
        float sinT = sqrt(saturate(1.0 - cosT2));
        float phi = 2.0 * PI * u.y;
        float3 H = T * (sinT * cos(phi)) + B * (sinT * sin(phi)) + N * cosT;
        L = reflect(-V, H);
    }

    if (dot(N, L) <= 0.0) return false;
    float pdf = disneyPdf(surf, w, N, T, B, V, L);
    if (pdf < 1e-7) return false;

    outDir = L;
    outWeight = evalDisneyBRDF(surf, N, T, B, V, L) * dot(N, L) / pdf;
    return true;
}

// ── Scene traversal with MASK (alpha-cutout) support ───────────────────────

// True when the hit survives the material's MASK alpha test — the same test
// the raster fragment's discard performs (cutoff in emissiveFactor.a; 0 =
// OPAQUE/BLEND, never cut). Requires the merged geometry + material table.
static bool hitAlphaPasses(uint instanceIndex, uint primitiveID, float2 barycentric,
                           device const InstanceData* instances,
                           device const MaterialData* materials,
                           device const VertexData* meshVertices,
                           device const uint* meshIndices,
                           const device MaterialTexs* materialTexs) {
    InstanceData inst = instances[instanceIndex];
    MaterialData mat = materials[inst.materialID];
    float cutoff = mat.emissiveFactor.a;
    if (cutoff <= 0.0) return true;

    uint b = inst.rtIndexOffset + primitiveID * 3u;
    uint i0 = meshIndices[b + 0u] + inst.rtVertexOffset;
    uint i1 = meshIndices[b + 1u] + inst.rtVertexOffset;
    uint i2 = meshIndices[b + 2u] + inst.rtVertexOffset;
    float2 bc = barycentric;
    float w0 = 1.0 - bc.x - bc.y;
    float2 uv = w0 * float2(meshVertices[i0].uv) + bc.x * float2(meshVertices[i1].uv)
              + bc.y * float2(meshVertices[i2].uv);

    constexpr sampler texSampler(address::repeat, filter::linear, mip_filter::linear);
    float alpha = materialTexs[inst.materialID].albedo.sample(texSampler, uv, level(0.0)).a;
    return alpha * mat.baseColorFactor.a >= cutoff;
}

// Any-hit occlusion query. When the scene contains MASK materials (and the
// geometry tables are bound), occlusion walks closest hits and passes through
// alpha-culled ones, so leaves shadow as leaves. maxDist caps the ray at the
// light — the light is not in the TLAS, and a wall BEHIND it must not occlude.
static bool shadowRayOccluded(instance_acceleration_structure TLAS,
                              float3 origin, float3 dir, float maxDist,
                              uint alphaTest,
                              device const InstanceData* instances,
                              device const MaterialData* materials,
                              device const VertexData* meshVertices,
                              device const uint* meshIndices,
                              const device MaterialTexs* materialTexs) {
    if (maxDist <= 0.001) return false;  // light effectively at the surface

    if (alphaTest == 0) {
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

    raytracing::intersector<raytracing::triangle_data, raytracing::instancing> occ;
    occ.assume_geometry_type(raytracing::geometry_type::triangle);
    for (uint step = 0; step < 8; step++) {
        raytracing::ray r;
        r.origin = origin;
        r.direction = dir;
        r.min_distance = 0.001;
        r.max_distance = maxDist;
        auto hit = occ.intersect(r, TLAS, 0xFF);
        if (hit.type != raytracing::intersection_type::triangle) return false;
        if (hitAlphaPasses(hit.user_instance_id, hit.primitive_id,
                           hit.triangle_barycentric_coord,
                           instances, materials, meshVertices, meshIndices, materialTexs)) {
            return true;
        }
        float advance = hit.distance + 0.001;
        origin += dir * advance;
        maxDist -= advance;
        if (maxDist <= 0.001) return false;
    }
    return true;  // pathological layering: call it occluded rather than loop on
}

// ── Analytic light helpers (conventions mirror 3d_pbr_lib.metal) ───────────

// The raster path's punctual falloff (CalculatePointLight): inverse square,
// windowed to exactly zero at the light's radius.
static float punctualAttenuation(float dist, float radius) {
    float atten = 1.0 / max(dist * dist, 1e-6);
    return atten * (1.0 - smoothstep(radius * 0.8, radius, dist));
}

// The raster path's spot cone (CalculateSpotLight): squared linear ramp
// between the outer (zero) and inner (full) half-angle cosines. `L` is the
// surface->light direction; light.direction points FROM the light.
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

// ── Hit resolution ──────────────────────────────────────────────────────────

// Resolve the hit triangle into the raster PBR's Surface plus a shading frame.
// Mirrors 3d_pbr_normal_mapped.metal's fragment setup: interpolated attributes,
// Gram-Schmidt TBN with the tangent-w bitangent sign and the degenerate-tangent
// fallback, normal map decoded as rgb*2-1 (the raster shader applies no
// normalScale either), material factors and sRGB linearization identical.
// Metal's intersector exposes no built-in normal/UV, so everything comes from
// the merged geometry (rtIndexOffset + primitive_id*3, rebased by
// rtVertexOffset) — the same fetch the RT reflection kernel performs.
static HitPoint resolveHitPoint(float3 rayDir, float3 rayOrigin, float hitDistance,
                                uint instanceIndex, uint primitiveID, float2 barycentric,
                                device const InstanceData* instances,
                                device const MaterialData* materials,
                                device const VertexData* meshVertices,
                                device const uint* meshIndices,
                                const device MaterialTexs* materialTexs,
                                uint hasBindlessGeo) {
    InstanceData inst = instances[instanceIndex];
    MaterialData mat = materials[inst.materialID];

    HitPoint hp;
    hp.position = rayOrigin + rayDir * hitDistance;
    hp.transmission = mat.transmission;

    Surface surf;
    surf.color = srgbToLinear(mat.baseColorFactor.rgb);
    // Baked AO approximates the occlusion this integrator computes for real;
    // applying both would double-darken (see the header).
    surf.ao = 1.0;
    surf.roughness = mat.roughnessFactor;
    surf.metallic = mat.metallicFactor;
    surf.emission = srgbToLinear(float3(mat.emissiveFactor.rgb)) * mat.emissiveStrength;
    surf.subsurface = mat.subsurface;
    surf.specular = mat.specular;
    surf.specular_tint = mat.specularTint;
    surf.anisotropic = mat.anisotropic;
    surf.sheen = mat.sheen;
    surf.sheen_tint = mat.sheenTint;
    surf.clearcoat = mat.clearcoat;
    surf.clearcoat_gloss = mat.clearcoatGloss;

    float3 N = -rayDir;  // fallback when no geometry tables are bound

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
        // model upper-3x3 for normals AND tangents — fine for the
        // rigid/uniform transforms the scene uses (same call the RT
        // reflection kernel makes; the raster path uses the normal matrix).
        N = normalize(model33 * nObj);
        // Shade the side the ray hit: single-sided geometry seen from behind
        // gets its frame flipped, or every lit term is black.
        if (dot(N, rayDir) > 0.0) N = -N;

        float2 uv = w0 * float2(v0.uv) + barycentric.x * float2(v1.uv)
                  + barycentric.y * float2(v2.uv);
        float4 tObj = w0 * float4(v0.tangent) + barycentric.x * float4(v1.tangent)
                    + barycentric.y * float4(v2.tangent);
        constexpr sampler texSampler(address::repeat, filter::linear, mip_filter::linear);
        // Base mip everywhere: path-traced rays carry no differentials, so
        // there is no footprint to pick a mip from. Texture aliasing resolves
        // through the sample count instead — that is what accumulation is for.
        texture2d<float, access::sample> texAlbedo    = materialTexs[inst.materialID].albedo;
        texture2d<float, access::sample> texNormal    = materialTexs[inst.materialID].normal;
        texture2d<float, access::sample> texMetallic  = materialTexs[inst.materialID].metallic;
        texture2d<float, access::sample> texRoughness = materialTexs[inst.materialID].roughness;
        texture2d<float, access::sample> texEmissive  = materialTexs[inst.materialID].emissive;

        surf.color = srgbToLinear(texAlbedo.sample(texSampler, uv, level(0.0)).rgb
                                  * mat.baseColorFactor.rgb);
        surf.roughness = texRoughness.sample(texSampler, uv, level(0.0)).g * mat.roughnessFactor;
        surf.metallic = texMetallic.sample(texSampler, uv, level(0.0)).b * mat.metallicFactor;
        surf.emission = srgbToLinear(texEmissive.sample(texSampler, uv, level(0.0)).rgb
                                     * mat.emissiveFactor.rgb) * mat.emissiveStrength;

        // TBN + normal map, exactly like the raster fragment (including the
        // degenerate-tangent guard that keeps NaNs out of the frame).
        float3 T = model33 * tObj.xyz;
        if (dot(T, T) > 1e-8) {
            T = normalize(T - dot(T, N) * N);
            float3 B = normalize(cross(N, T) * tObj.w);
            float3x3 TBN = float3x3(T, B, N);
            float3 mapped = normalize(TBN * normalize(
                texNormal.sample(texSampler, uv, level(0.0)).rgb * 2.0 - 1.0));
            hp.N = mapped;
            hp.T = T;
            hp.B = B;
        } else {
            hp.N = N;
            buildBasis(N, hp.T, hp.B);
        }
    } else {
        hp.N = N;
        buildBasis(N, hp.T, hp.B);
    }

    surf.metallic = saturate(surf.metallic);
    hp.surf = surf;
    return hp;
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

    const uint alphaTest = (params.hasAlphaMask != 0 && params.hasBindlessGeo != 0) ? 1u : 0u;

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
            // MASK pass-through: a hit that fails its material's alpha test is
            // not a surface — step past it and re-cast (bounded, so a stack of
            // cutout cards cannot spin the kernel).
            for (uint step = 0; step < 8 && alphaTest != 0 &&
                 hit.type == raytracing::intersection_type::triangle &&
                 !hitAlphaPasses(hit.user_instance_id, hit.primitive_id,
                                 hit.triangle_barycentric_coord,
                                 instances, materials, meshVertices, meshIndices,
                                 materialTexs); step++) {
                r.origin = r.origin + rayDir * (hit.distance + 0.001);
                hit = primary.intersect(r, TLAS, 0xFF);
            }

            if (hit.type != raytracing::intersection_type::triangle) {
                radiance += throughput * envMap.sample(envSampler, rayDir, level(0.0)).rgb
                          * params.envIntensity;
                break;
            }

            HitPoint hp = resolveHitPoint(rayDir, r.origin, hit.distance,
                                          hit.user_instance_id, hit.primitive_id,
                                          hit.triangle_barycentric_coord,
                                          instances, materials, meshVertices, meshIndices,
                                          materialTexs, params.hasBindlessGeo);

            radiance += throughput * hp.surf.emission;

            float3 V = -rayDir;
            float3 N = hp.N;

            // KHR_materials_transmission: stochastic thin-glass lobe matching
            // the raster refraction pass's convention — IOR 1.5, one
            // interface, TIR falls back to reflect, tinted by base color.
            // Branch probability = the material's transmission share, so no
            // throughput division is needed (t·glass + (1-t)·opaque mixture).
            float pTransmit = hp.transmission * (1.0 - hp.surf.metallic);
            if (pTransmit > 0.0 && randomNext(rngState) < pTransmit) {
                float cosI = saturate(dot(N, V));
                float F = 0.04 + 0.96 * pow(1.0 - cosI, 5.0);
                float3 newDir;
                if (randomNext(rngState) < F) {
                    newDir = reflect(rayDir, N);
                } else {
                    newDir = refract(rayDir, N, 1.0 / 1.5);
                    if (dot(newDir, newDir) < 1e-8) newDir = reflect(rayDir, N);  // TIR
                }
                throughput *= hp.surf.color;
                rayOrigin = hp.position + newDir * 0.001
                          + (dot(newDir, N) > 0.0 ? N : -N) * params.rayBias;
                rayDir = newDir;
                continue;  // a glass pass consumes a bounce
            }

            // ── Next-event estimation over every analytic light, one shadow
            // ray each. None of these lights exist in the TLAS, so a BSDF ray
            // can never hit one — NEE is each light's only estimator and no
            // MIS weight applies. Conventions mirror 3d_pbr_lib.metal (see the
            // header) so the converged image is comparable with the raster
            // frame.
            const float3 shadowOrigin = hp.position + N * params.rayBias;

            for (uint li = 0; li < params.dirLightCount; li++) {
                DirLight light = dirLights[li];
                float3 sunDir = normalize(-light.direction);
                float2 rnd = float2(randomNext(rngState), randomNext(rngState));
                float3 L = sampleSunDirection(sunDir, params.sunAngularRadius, rnd);

                if (dot(N, L) <= 0.0) continue;
                float3 f = evalDisneyBRDF(hp.surf, N, hp.T, hp.B, V, L);
                if (all(f <= float3(0.0))) continue;
                if (shadowRayOccluded(TLAS, shadowOrigin, L, params.rayMaxDistance, alphaTest,
                                      instances, materials, meshVertices, meshIndices,
                                      materialTexs)) continue;

                radiance += throughput * f * dot(N, L) * light.color * light.intensity;
            }

            for (uint li = 0; li < params.pointLightCount; li++) {
                PointLight light = pointLights[li];
                float3 toLight = light.position - hp.position;
                float dist = length(toLight);
                // The window function is exactly zero at radius, so this skip
                // is exact — it is the raster falloff's own boundary.
                if (dist >= light.radius || dist < 1e-4) continue;
                float3 L = toLight / dist;

                if (dot(N, L) <= 0.0) continue;
                float3 f = evalDisneyBRDF(hp.surf, N, hp.T, hp.B, V, L);
                if (all(f <= float3(0.0))) continue;
                if (shadowRayOccluded(TLAS, shadowOrigin, L, dist - 0.005, alphaTest,
                                      instances, materials, meshVertices, meshIndices,
                                      materialTexs)) continue;

                radiance += throughput * f * dot(N, L) * light.color * light.intensity
                          * punctualAttenuation(dist, light.radius);
            }

            for (uint li = 0; li < params.spotLightCount; li++) {
                SpotLight light = spotLights[li];
                float3 toLight = light.position - hp.position;
                float dist = length(toLight);
                if (dist >= light.radius || dist < 1e-4) continue;
                float3 L = toLight / dist;

                float cone = spotConeFactor(L, light.direction, light.cosInner, light.cosOuter);
                if (cone <= 0.0) continue;

                if (dot(N, L) <= 0.0) continue;
                float3 f = evalDisneyBRDF(hp.surf, N, hp.T, hp.B, V, L);
                if (all(f <= float3(0.0))) continue;
                if (shadowRayOccluded(TLAS, shadowOrigin, L, dist - 0.005, alphaTest,
                                      instances, materials, meshVertices, meshIndices,
                                      materialTexs)) continue;

                radiance += throughput * f * dot(N, L) * light.color * light.intensity
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

                float3 toQ = q - hp.position;
                float distSq = max(dot(toQ, toQ), 1e-6);
                float dist = sqrt(distSq);
                float3 L = toQ / dist;

                if (dot(N, L) <= 0.0) continue;
                // Double-sided emitter, like the raster polygon formula.
                float cosLight = abs(dot(normalize(cross(lr, lu)), L));
                if (cosLight < 1e-4) continue;

                float3 f = evalDisneyBRDF(hp.surf, N, hp.T, hp.B, V, L);
                if (all(f <= float3(0.0))) continue;
                if (shadowRayOccluded(TLAS, shadowOrigin, L, dist - 0.005, alphaTest,
                                      instances, materials, meshVertices, meshIndices,
                                      materialTexs)) continue;

                float area = 4.0 * light.halfWidth * light.halfHeight;
                // color * intensity / PI: the emitted radiance at which this
                // estimator's expectation equals the raster path's exact
                // solid-angle diffuse term (see header). Video-textured lights
                // emit their flat color — the video texture is not bound here.
                float3 emitted = float3(light.color) * (light.intensity / PI);
                radiance += throughput * f * dot(N, L) * emitted * (area * cosLight / distSq);
            }

            if (bounce == params.maxBounces) break;

            float3 nextDir, weight;
            if (!sampleDisney(hp.surf, N, hp.T, hp.B, V, rngState, nextDir, weight)) break;
            throughput *= weight;
            if (all(throughput <= float3(0.0))) break;

            // Russian roulette once the path has had a chance to gather light.
            // Survivors are scaled by 1/p, which keeps the estimator unbiased.
            if (bounce >= 2) {
                float p = clamp(max(throughput.r, max(throughput.g, throughput.b)), 0.05, 1.0);
                if (randomNext(rngState) > p) break;
                throughput /= p;
            }

            rayOrigin = hp.position + N * params.rayBias;
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
