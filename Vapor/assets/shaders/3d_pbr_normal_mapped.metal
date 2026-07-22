#include <metal_stdlib>
using namespace metal;
#include "Res/shaders/3d_common.metal" // TODO: use more robust include path

// Bindless-materials specialization (the ICB draw mode). When the pipeline is
// built with kBindlessMaterials=true (function constant 0, RHI
// ShaderDesc::bindlessMaterials), the fragment reads its six material textures
// from the argument table at buffer(13) indexed by the materialID varying,
// instead of the per-draw bound slots 0-5 — required because a single
// executeCommandsInBuffer can't rebind textures between draws. Unspecialized
// pipelines see the constant as undefined -> false and keep the bound path.
constant bool kBindlessMaterialsSet [[function_constant(0)]];
constant bool kBindlessMaterials = is_function_constant_defined(kBindlessMaterialsSet) && kBindlessMaterialsSet;
constant bool kBoundMaterials = !kBindlessMaterials;

// One entry per material in the bindless table (matches the RHI's
// createTextureArgumentTable(..., texturesPerEntry=6) slot order).
struct MaterialTexs {
    texture2d<float, access::sample> albedo    [[id(0)]];
    texture2d<float, access::sample> normal    [[id(1)]];
    texture2d<float, access::sample> metallic  [[id(2)]];
    texture2d<float, access::sample> roughness [[id(3)]];
    texture2d<float, access::sample> occlusion [[id(4)]];
    texture2d<float, access::sample> emissive  [[id(5)]];
};

// The bindless variant must take EVERY texture through argument buffers: a
// pipeline built with supportIndirectCommandBuffers rejects any direct
// texture/sampler argument on the fragment function ("Fragment shader cannot
// be used with indirect command buffers"). This single-entry table carries the
// per-frame system textures (slot order = the renderer's Metal contract 6-15).
struct SystemTexs {
    texture2d<float, access::sample>     texAO          [[id(0)]];
    texture2d<float, access::sample>     texShadow      [[id(1)]];
    texturecube<float, access::sample>   irradianceMap  [[id(2)]];
    texturecube<float, access::sample>   prefilterMap   [[id(3)]];
    texture2d<float, access::sample>     brdfLUT        [[id(4)]];
    texture2d<float, access::sample>     rectLightVideo [[id(5)]];
    depth2d_array<float, access::sample> pssmShadowMaps [[id(6)]];
    texture2d<float, access::sample>     texPointShadow [[id(7)]];
    texture2d<float, access::sample>     gibsGI         [[id(8)]];
    texture2d<float, access::sample>     texSSCS        [[id(9)]];
    // RT reflection/refraction results (bindless path): the ICB fragment can't
    // take direct texture args, so these join the system table like the rest.
    texture2d<float, access::sample>     texReflection  [[id(10)]];
    texture2d<float, access::sample>     texRefraction  [[id(11)]];
};

struct RasterizerData {
    float4 position [[position]];
    float2 uv;
    float4 worldPosition;
    float4 worldNormal;
    float4 worldTangent;
    float3 scaledLocalPos;
    float3 localNormal;
    // Material is fetched by id in the fragment (materials[materialID]), NOT
    // passed through inter-stage: the full 112-byte MaterialData overflowed
    // Metal's per-vertex output capacity, corrupting varyings and dropping
    // geometry (the Vulkan RHIMain.frag already fetches by fragMaterialID).
    uint materialID [[flat]];  // bindless table index (ICB mode) + material fetch index
};

struct Surface {
    float3 color;
    float ao;
    float roughness;
    float metallic;
    float3 emission;
    float subsurface;
    float specular;
    float specular_tint;
    float anisotropic;
    float sheen;
    float sheen_tint;
    float clearcoat;
    float clearcoat_gloss;
};

float GTR1(float nh, float a) {
    if (a >= 1.0) return 1.0 / PI;
    float a2 = a * a;
    float t = 1.0 + (a2 - 1.0) * nh * nh;
    return (a2 - 1.0) / (PI * log(a2) * t);
}

float GTR2_aniso(float nh, float hx, float hy, float ax, float ay) {
    float t = (hx * hx) / (ax * ax) + (hy * hy) / (ay * ay) + nh * nh;
    return 1.0 / (PI * ax * ay * t * t);
}

float TrowbridgeReitzGGX(float nh, float r) {
    float a = r * r; // TODO: use r + 0.01?
    float a2 = a * a;
    float nh2 = nh * nh;
    float t2 = (nh2 * (a2 - 1.0) + 1.0) * (nh2 * (a2 - 1.0) + 1.0);
    return a2 / (PI * t2);
}

float SmithsSchlickGGX(float nv, float nl, float r) {
    float k = (r + 1.0) * (r + 1.0) / 8.0;
    float ggx1 = nv / (nv * (1.0 - k) + k);
    float ggx2 = nl / (nl * (1.0 - k) + k);
    return ggx1 * ggx2;
}

float SmithGGX(float u, float r) {
    float a = r * r;
    float b = u * u;
    return 1.0 / (u + sqrt(a + b - a * b));
}

float SmithGGX_aniso(float u, float vx, float vy, float ax, float ay) {
    float t = vx * vx * ax * ax + vy * vy * ay * ay + u * u;
    return 1.0 / (u + sqrt(t));
}

float FresnelApprox(float u) {
    return pow(1.0 + 0.0001 - u, 5.0);
}

float luminance(float3 color) {
    return dot(color, float3(0.3, 0.6, 0.1));
}

float3 CookTorranceBRDF(float3 norm, float3 tangent, float3 bitangent, float3 lightDir, float3 viewDir, Surface surf) {
    float3 halfway = normalize(lightDir + viewDir);
    float nv = max(dot(norm, viewDir), 0.0);
    float nl = max(dot(norm, lightDir), 0.0);
    float nh = max(dot(norm, halfway), 0.0);
    // float vh = max(dot(viewDir, halfway), 0.0);
    float lh = max(dot(lightDir, halfway), 0.0);
    float lum = luminance(surf.color);
    float3 tint = lum > 0.0 ? surf.color / lum : float3(1);
    float3 spec0 = mix(surf.specular * 0.08 * mix(float3(1), tint, surf.specular_tint), surf.color, surf.metallic);
    float fh = FresnelApprox(lh);
    float fl = FresnelApprox(nl);
    float fv = FresnelApprox(nv);
    float fss90 = lh * lh * surf.roughness;
    // diffuse
    float fd90 = 0.5 + 2.0 * fss90;
    float kd = mix(1.0, fd90, fl) * mix(1.0, fd90, fv);
    // float3 diffuse = kd * surf.color / PI;
    // subsurface
    float fss = mix(1.0, fss90, fl) * mix(1.0, fss90, fv);
    float ss = 1.25 * (fss * (1.0 / (nl + nv + 0.0001) - 0.5) + 0.5);
    // specular
    float aspect = sqrt(1.0 - surf.anisotropic * .9);
    float ax = max(.001, surf.roughness * surf.roughness / aspect);
    float ay = max(.001, surf.roughness * surf.roughness * aspect);
    float3 x = tangent;
    float3 y = bitangent;
    float hx = dot(halfway, x);
    float hy = dot(halfway, y);
    float lx = dot(lightDir, x);
    float ly = dot(lightDir, y);
    float vx = dot(viewDir, x);
    float vy = dot(viewDir, y);
    float D = GTR2_aniso(nh, hx, hy, ax, ay); // TrowbridgeReitzGGX(nh, surf.roughness);
    float G = SmithGGX_aniso(nl, lx, ly, ax, ay) * SmithGGX_aniso(nv, vx, vy, ax, ay); // SmithsSchlickGGX(nv, nl, surf.roughness + 0.01) / max(4.0 * nv * nl, 0.0001);
    float3 F = mix(spec0, float3(1.0), fh);
    float3 specular = D * G * F;
    // sheen
    float3 sheen = fh * surf.sheen * mix(float3(1), tint, surf.sheen_tint);
    // clearcoat
    float Dr = GTR1(nh, mix(.1, .001, surf.clearcoat_gloss));
    float Fr = mix(.04, 1.0, fh);
    float Gr = SmithGGX(nl, .25) * SmithGGX(nv, .25);
    float3 clearcoat = 0.25 * float3(surf.clearcoat) * Dr * Fr * Gr;

    return ((mix(kd, ss, surf.subsurface) * surf.color / PI + sheen) * (1.0 - surf.metallic) + specular + clearcoat) * nl;
}

float3 CalculateDirectionalLight(DirLight light, float3 norm, float3 tangent, float3 bitangent, float3 viewDir, Surface surf) {
    float3 lightDir = normalize(-light.direction);
    float3 radiance = light.color * light.intensity;

    // NdotL is already folded into CookTorranceBRDF's `* nl` — don't apply it
    // twice (that squared the cosine and darkened grazing faces).
    return CookTorranceBRDF(norm, tangent, bitangent, lightDir, viewDir, surf) * radiance;
}

float3 CalculatePointLight(PointLight light, float3 norm, float3 tangent, float3 bitangent, float3 viewDir, Surface surf, float3 fragPos) {
    float3 lightDir = normalize(light.position - fragPos);
    float dist = distance(light.position, fragPos);
    float attenuation = 1.0 / (dist * dist);
    attenuation *= 1.0 - smoothstep(light.radius * 0.8, light.radius, dist);
    float3 radiance = attenuation * light.color * light.intensity;

    // NdotL is already folded into CookTorranceBRDF's `* nl` — don't apply it
    // twice (that squared the cosine and darkened grazing faces).
    return CookTorranceBRDF(norm, tangent, bitangent, lightDir, viewDir, surf) * radiance;
}

// Spot: a point light windowed by a smooth cone falloff between the inner
// (full intensity) and outer (zero) half-angle cosines.
float3 CalculateSpotLight(SpotLight light, float3 norm, float3 tangent, float3 bitangent, float3 viewDir, Surface surf, float3 fragPos) {
    float3 lightDir = normalize(light.position - fragPos);
    float dist = distance(light.position, fragPos);
    float attenuation = 1.0 / (dist * dist);
    attenuation *= 1.0 - smoothstep(light.radius * 0.8, light.radius, dist);
    float cosAngle = dot(-lightDir, light.direction);
    float cone = clamp((cosAngle - light.cosOuter) / max(light.cosInner - light.cosOuter, 1e-4), 0.0, 1.0);
    attenuation *= cone * cone;
    float3 radiance = attenuation * light.color * light.intensity;

    // NdotL is already folded into CookTorranceBRDF's `* nl` — don't apply it
    // twice (that squared the cosine and darkened grazing faces).
    return CookTorranceBRDF(norm, tangent, bitangent, lightDir, viewDir, surf) * radiance;
}

// ── Rect area light (diffuse + specular) ─────────────────────────────────────

// Exact diffuse irradiance from a quad via the polygon solid-angle edge formula
// (Baum et al. 1989 / Arvo 1994).  Returns irradiance ∈ [0, 1/2].
float EvalRectLightDiffuse(float3 N, float3 fragPos, RectLight light) {
    // Hoist packed_float3 fields into aligned locals before doing math on them.
    float3 lp = float3(light.position);
    float3 lr = float3(light.right);
    float3 lu = float3(light.up);
    float3 corners[4] = {
        lp + lr * light.halfWidth + lu * light.halfHeight,
        lp - lr * light.halfWidth + lu * light.halfHeight,
        lp - lr * light.halfWidth - lu * light.halfHeight,
        lp + lr * light.halfWidth - lu * light.halfHeight,
    };
    float3 sum = float3(0.0);
    for (int i = 0; i < 4; i++) {
        float3 v0  = normalize(corners[i]       - fragPos);
        float3 v1  = normalize(corners[(i+1)%4] - fragPos);
        float3 c   = cross(v0, v1);
        float  len = length(c);
        if (len < 1e-6) continue;
        float theta = atan2(len, dot(v0, v1));
        sum += (theta / len) * c;
    }
    return max(0.0f, dot(sum, N)) / (2.0f * PI);
}

// Specular contribution via Most Representative Point (Karis, SIGGRAPH 2013).
// Finds the closest point on the rect to the reflection ray and evaluates GGX
// with an area-corrected roughness for energy conservation.
float3 EvalRectLightSpecular(float3 N, float3 fragPos, float3 viewDir, RectLight light, Surface surf) {
    float3 lp = float3(light.position);
    float3 lr = float3(light.right);
    float3 lu = float3(light.up);
    float3 refl       = reflect(-viewDir, N);
    float3 lightNorm  = cross(lr, lu); // unnormalized plane normal

    float denom   = dot(lightNorm, refl);
    float3 repPt  = lp;
    if (abs(denom) > 1e-5) {
        float t = dot(lp - fragPos, lightNorm) / denom;
        if (t > 0.0) {
            float3 hit = fragPos + refl * t;
            float3 rel = hit - lp;
            float u    = clamp(dot(rel, lr), -light.halfWidth,  light.halfWidth);
            float v    = clamp(dot(rel, lu), -light.halfHeight, light.halfHeight);
            repPt = lp + lr * u + lu * v;
        }
    }

    float3 lightDir = normalize(repPt - fragPos);
    float  nDotL    = max(dot(N, lightDir), 0.0f);
    if (nDotL <= 0.0f) return float3(0.0);

    float dist  = length(repPt - fragPos);
    float area  = 4.0f * light.halfWidth * light.halfHeight;
    float alpha = surf.roughness * surf.roughness;
    float alphaPrime = saturate(alpha + area / max(2.0f * PI * dist * dist, 1e-6f));
    float r = sqrt(alphaPrime);

    float3 halfway = normalize(lightDir + viewDir);
    float  nh  = max(dot(N, halfway),    0.0f);
    float  vh  = max(dot(viewDir, halfway), 0.0f);
    float  nv  = max(dot(N, viewDir),    1e-4f);
    float  D   = TrowbridgeReitzGGX(nh, r);
    float  G   = SmithsSchlickGGX(nv, nDotL, r);
    float  lum = luminance(surf.color);
    float3 tint = lum > 0.0f ? surf.color / lum : float3(1.0f);
    float3 F0   = mix(surf.specular * 0.08f * mix(float3(1.0f), tint, surf.specular_tint), surf.color, surf.metallic);
    float3 F    = F0 + (1.0f - F0) * pow(1.0f - vh, 5.0f);

    return D * G * F / (4.0f * nv * nDotL + 1e-6f);
}

// UV of a world-space position projected onto the rect light face [0,1]².
float2 RectLightUV(RectLight light, float3 worldPos) {
    float3 rel = worldPos - float3(light.position);
    float u = dot(rel, float3(light.right)) / light.halfWidth  * 0.5f + 0.5f;
    float v = dot(rel, float3(light.up))    / light.halfHeight * 0.5f + 0.5f;
    return saturate(float2(u, v));
}

// Effective radiance colour of the light.  For solid-colour lights this is just
// light.color.  For video lights, five representative points are sampled across
// the rect and averaged, giving a spatially-weighted diffuse colour.
float3 RectLightColor(RectLight light, float3 fragPos,
                      texture2d<float, access::sample> videoTex) {
    if (light.useVideoTexture == 0) {
        return float3(light.color);
    }
    constexpr sampler clampS(address::clamp_to_edge, filter::linear);
    float3 lp = float3(light.position);
    float3 lr = float3(light.right);
    float3 lu = float3(light.up);
    // 5-point stratified sample across the rect face
    float3 pts[5] = {
        lp,
        lp + lr * light.halfWidth  * 0.5f,
        lp - lr * light.halfWidth  * 0.5f,
        lp + lu * light.halfHeight * 0.5f,
        lp - lu * light.halfHeight * 0.5f,
    };
    float3 col = float3(0.0f);
    for (int i = 0; i < 5; i++) {
        col += srgbToLinear(videoTex.sample(clampS, RectLightUV(light, pts[i])).rgb);
    }
    return col / 5.0f;
}

float3 CalculateRectLight(RectLight light, float3 N, float3 fragPos, float3 viewDir,
                          Surface surf, texture2d<float, access::sample> videoTex) {
    float3 emissive = RectLightColor(light, fragPos, videoTex);
    float3 radiance = emissive * light.intensity;

    float  diffuseGeo = EvalRectLightDiffuse(N, fragPos, light);
    float3 specular   = EvalRectLightSpecular(N, fragPos, viewDir, light, surf);

    // Fresnel-based kD/kS split (metals have no diffuse)
    float3 F0  = mix(surf.specular * 0.08f * float3(1.0f), surf.color, surf.metallic);
    float3 kD  = (float3(1.0f) - F0) * (1.0f - surf.metallic);

    return (kD * surf.color / PI * diffuseGeo + specular) * radiance;
}

// Fresnel-Schlick approximation with roughness for IBL
float3 FresnelSchlickRoughness(float cosTheta, float3 F0, float roughness) {
    return F0 + (max(float3(1.0 - roughness), F0) - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

// Calculate IBL (Image-Based Lighting) contribution
float3 CalculateIBL(
    float3 norm,
    float3 viewDir,
    Surface surf,
    texturecube<float, access::sample> irradianceMap,
    texturecube<float, access::sample> prefilterMap,
    texture2d<float, access::sample> brdfLUT
) {
    constexpr sampler cubeSampler(filter::linear, mip_filter::linear);
    constexpr sampler lutSampler(filter::linear, address::clamp_to_edge);

    float NdotV = max(dot(norm, viewDir), 0.0);

    // Calculate F0 (reflectance at normal incidence)
    float3 F0 = float3(0.04);
    F0 = mix(F0, surf.color, surf.metallic);

    // Fresnel term with roughness
    float3 F = FresnelSchlickRoughness(NdotV, F0, surf.roughness);

    // Diffuse and specular weights
    float3 kS = F;
    float3 kD = (1.0 - kS) * (1.0 - surf.metallic);

    // Diffuse IBL: sample irradiance map
    float3 irradiance = irradianceMap.sample(cubeSampler, norm).rgb;
    float3 diffuseIBL = irradiance * surf.color * kD;

    // Specular IBL: sample pre-filtered environment map
    float3 R = reflect(-viewDir, norm);
    const float MAX_REFLECTION_LOD = 4.0;
    float mipLevel = surf.roughness * MAX_REFLECTION_LOD;
    float3 prefilteredColor = prefilterMap.sample(cubeSampler, R, level(mipLevel)).rgb;

    // BRDF lookup
    float2 brdf = brdfLUT.sample(lutSampler, float2(NdotV, surf.roughness)).rg;
    float3 specularIBL = prefilteredColor * (F * brdf.x + brdf.y);

    return (diffuseIBL + specularIBL) * surf.ao;
}

vertex RasterizerData vertexMain(
    uint vertexID [[vertex_id]],
    constant CameraData& camera [[buffer(0)]],
    constant MaterialData* materials [[buffer(1)]],
    constant InstanceData* instances [[buffer(2)]],
    device const VertexData* in [[buffer(3)]],
    constant uint& instanceID [[buffer(4)]],
    uint baseInstance [[base_instance]]
) {
    RasterizerData vert;
    // Effective instance index. Normal/per-object draws pass it via buffer(4)
    // with baseInstance 0 (no-op); single-call MDI can't set a per-object
    // constant, so it passes instanceID 0 and carries the index in the draw
    // command's baseInstance.
    uint iid = instanceID + baseInstance;
    uint actualVertexID = instances[iid].vertexOffset + vertexID;
    float4x4 model = instances[iid].model;
    float3x3 normalMatrix = transpose(inverse(float3x3(
        model[0].xyz,
        model[1].xyz,
        model[2].xyz
    )));
    // Caution: worldNormal and worldTangent are not normalized yet, and they can be affected by model scaling
    vert.worldNormal = float4(normalMatrix * float3(in[actualVertexID].normal), 0.0);
    vert.worldTangent = float4(normalMatrix * in[actualVertexID].tangent.xyz, in[actualVertexID].tangent.w);
    vert.worldPosition = model * float4(in[actualVertexID].position, 1.0);
    vert.position = camera.proj * camera.view * vert.worldPosition;
    vert.uv = in[actualVertexID].uv;
    // Material fetched in the fragment by this id (not passed through
    // inter-stage — see RasterizerData).
    vert.materialID = instances[iid].materialID;
    
    // Pass scaled local position and local normal for Object Space Triplanar
    float3 scale = float3(length(model[0].xyz), length(model[1].xyz), length(model[2].xyz));
    vert.scaledLocalPos = float3(in[actualVertexID].position) * scale;
    vert.localNormal = float3(in[actualVertexID].normal);
    
    return vert;
}

// ── Terrain surface (shaderModel == 1) — MSL twin of RHIMain.frag's branch ──
// Faithful port of Atmospheric's terrain.frag 4-layer splat: weights recomputed
// per fragment from height/slope + world-space FBm breakup (defaultSplat rules,
// no splat texture), detail layers tiled in world space. Layer order/frequency
// (repeats per metre): 0 grass 0.25, 1 rock 0.046875, 2 dirt 0.125,
// 3 snow 0.078125.
constant float4 kTerrainLayerFreq = float4(0.25, 0.046875, 0.125, 0.078125);
constant uint kTerrainSplatSeed = 7u;

inline uint trgHash2(int x, int y, uint seed) {
    uint h = uint(x) * 374761393u + uint(y) * 668265263u + seed * 2654435761u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}
inline float trgHash01(int x, int y, uint seed) { return float(trgHash2(x, y, seed) >> 8) * (1.0 / 16777216.0); }
inline float trgSmooth(float t) { return t * t * (3.0 - 2.0 * t); }

static float trgWorldFBm(float2 p, float wavelength, int octaves, uint seed) {
    float sum = 0.0, amp = 0.5, freq = 1.0 / wavelength;
    for (int k = 0; k < octaves; ++k) {
        float u = p.x * freq, v = p.y * freq;
        int xi = int(floor(u)), yi = int(floor(v));
        float fx = trgSmooth(u - float(xi)), fy = trgSmooth(v - float(yi));
        uint sd = seed + uint(k) * 131u;
        float a = trgHash01(xi, yi, sd),     b = trgHash01(xi + 1, yi, sd);
        float c = trgHash01(xi, yi + 1, sd), d = trgHash01(xi + 1, yi + 1, sd);
        sum += amp * mix(mix(a, b, fx), mix(c, d, fx), fy);
        amp *= 0.5; freq *= 2.0;
    }
    return sum;
}

static float4 trgSplatWeights(float height01, float slope, float2 worldXZ) {
    float b1 = trgWorldFBm(worldXZ, 180.0, 3, kTerrainSplatSeed);
    float b2 = trgWorldFBm(worldXZ, 45.0, 3, kTerrainSplatSeed + 101u);
    float rock = smoothstep(0.55, 1.05, slope + 0.25 * (b1 - 0.5));
    float snowline = 0.62 + 0.08 * (b2 - 0.5);
    float snow = smoothstep(snowline, snowline + 0.16, height01) * (1.0 - 0.85 * rock);
    float dirt = 0.55 * smoothstep(0.5, 0.75, b2) * smoothstep(0.18, 0.45, slope + 0.2 * (b1 - 0.5));
    dirt += 0.6 * smoothstep(0.10, 0.04, height01);
    dirt = clamp(dirt, 0.0, 1.0) * (1.0 - rock) * (1.0 - snow);
    float grass = max(1.0 - rock - snow - dirt, 0.0);
    float4 w = float4(grass, rock, dirt, snow);
    return w / max(w.x + w.y + w.z + w.w, 1e-4);
}

// Terrain height field — MSL twin of RHIMain.frag's trHeightAt, a byte-for-byte
// port of TerrainWorld::heightAt (terrain_world.cpp). Lets the fragment stage
// reconstruct a per-pixel surface normal from the same noise the mesh is built
// on, restoring the octaves the coarse LOD mesh vertices smooth away. Params
// arrive packed in the terrain material's unused Disney lobe fields.
inline float trhHashNoise(int x, int y, uint seed) {
    uint h = uint(x) * 374761393u + uint(y) * 668265263u + seed * 3266489917u;
    h = (h ^ (h >> 13)) * 1274126177u;
    h ^= h >> 16;
    return float(h & 0xFFFFu) / 65535.0;
}
inline float trhGradDot(int xi, int zi, float2 offset, uint seed) {
    float2 g = float2(trhHashNoise(xi, zi, seed) - 0.5, trhHashNoise(xi, zi, seed ^ 0x9E3779B9u) - 0.5);
    float len = length(g);
    if (len < 1e-6) return offset.x;
    return dot(g / len, offset);
}
inline float trhGradNoise2(float2 p, uint seed) {
    float2 pf = floor(p);
    float2 f = p - pf;
    float2 u = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);  // quintic fade
    int xi = int(pf.x), zi = int(pf.y);
    float a = trhGradDot(xi, zi, f, seed);
    float b = trhGradDot(xi + 1, zi, f - float2(1.0, 0.0), seed);
    float c = trhGradDot(xi, zi + 1, f - float2(0.0, 1.0), seed);
    float d = trhGradDot(xi + 1, zi + 1, f - float2(1.0, 1.0), seed);
    return mix(mix(a, b, u.x), mix(c, d, u.x), u.y);
}
inline float trhHeightAt(float2 xz, float noiseFreq, int octaves, uint seed, float heightScale) {
    float2 p = xz * noiseFreq;
    float sum = 0.0, amp = 0.5;
    for (int i = 0; i < octaves; ++i) {
        sum += amp * trhGradNoise2(p, seed + uint(i) * 101u);
        p *= 2.0;
        amp *= 0.5;
    }
    return clamp(0.5 + sum * 1.2, 0.0, 1.0) * heightScale;
}

static void trgShadeTerrain(float3 worldPos, float noiseFreq, int octaves, uint seed, float heightScale,
                            float height01,
                            texture2d_array<float, access::sample> detailAlbedo,
                            texture2d_array<float, access::sample> detailNormal,
                            thread float3& outAlbedo, thread float3& outN) {
    constexpr sampler ts(address::repeat, filter::linear, mip_filter::linear);
    // Central-difference normal at the pixel's world-space footprint (>= 1 m),
    // so distant terrain band-limits the noise (no shimmer) while near terrain
    // resolves the finest octave. Sign matches buildTileGeometry's vertex normal.
    float fp = max(max(abs(dfdx(worldPos.x)), abs(dfdy(worldPos.x))),
                   max(abs(dfdx(worldPos.z)), abs(dfdy(worldPos.z))));
    float d = clamp(fp, 1.0, 64.0);
    float hl = trhHeightAt(worldPos.xz - float2(d, 0.0), noiseFreq, octaves, seed, heightScale);
    float hr = trhHeightAt(worldPos.xz + float2(d, 0.0), noiseFreq, octaves, seed, heightScale);
    float hb = trhHeightAt(worldPos.xz - float2(0.0, d), noiseFreq, octaves, seed, heightScale);
    float ht = trhHeightAt(worldPos.xz + float2(0.0, d), noiseFreq, octaves, seed, heightScale);
    float3 baseN = normalize(float3(hl - hr, 2.0 * d, hb - ht));

    float slope = length(baseN.xz) / max(baseN.y, 1e-3);  // rise/run
    float4 w = trgSplatWeights(height01, slope, worldPos.xz);
    float2 wp = worldPos.xz;
    float3 c = float3(0.0);
    float3 dn = float3(0.0);
    for (int i = 0; i < 4; ++i) {
        float2 uv = wp * kTerrainLayerFreq[i];
        c  += w[i] * pow(detailAlbedo.sample(ts, uv, i).rgb, float3(2.2));
        dn += w[i] * (detailNormal.sample(ts, uv, i).xyz * 2.0 - 1.0);
    }
    outAlbedo = c;
    dn = normalize(dn + float3(0.0, 0.0, 1e-4));
    float3 nn = baseN;
    float3 T = normalize(float3(1.0, 0.0, 0.0) - nn * nn.x);
    float3 B = cross(nn, T);
    outN = normalize(T * dn.x + B * dn.y + nn * dn.z);
}

fragment float4 fragmentMain(
    RasterizerData in [[stage_in]],
    // Per-draw material textures — bound path only. The bindless
    // specialization disables these and reads materialTexs instead.
    texture2d<float, access::sample> texAlbedo [[texture(0), function_constant(kBoundMaterials)]],
    texture2d<float, access::sample> texNormal [[texture(1), function_constant(kBoundMaterials)]],
    texture2d<float, access::sample> texMetallic [[texture(2), function_constant(kBoundMaterials)]],
    texture2d<float, access::sample> texRoughness [[texture(3), function_constant(kBoundMaterials)]],
    texture2d<float, access::sample> texOcclusion [[texture(4), function_constant(kBoundMaterials)]],
    texture2d<float, access::sample> texEmissive [[texture(5), function_constant(kBoundMaterials)]],
    const device MaterialTexs* materialTexs [[buffer(13), function_constant(kBindlessMaterials)]],
    // System textures (Metal contract slots 6-15). Direct arguments on the
    // bound path only; the bindless path reads the same set from systemTexs
    // (locals with the original names are resolved at the top of the body, so
    // everything below is shared).
    texture2d<float, access::sample> texAOArg [[texture(6), function_constant(kBoundMaterials)]],
    texture2d<float, access::sample> texShadowArg [[texture(7), function_constant(kBoundMaterials)]],
    texturecube<float, access::sample> irradianceMapArg [[texture(8), function_constant(kBoundMaterials)]],
    texturecube<float, access::sample> prefilterMapArg [[texture(9), function_constant(kBoundMaterials)]],
    texture2d<float, access::sample> brdfLUTArg [[texture(10), function_constant(kBoundMaterials)]],
    texture2d<float, access::sample> rectLightVideoArg [[texture(11), function_constant(kBoundMaterials)]],
    depth2d_array<float, access::sample> pssmShadowMapsArg [[texture(12), function_constant(kBoundMaterials)]],
    texture2d<float, access::sample> texPointShadowArg [[texture(13), function_constant(kBoundMaterials)]],
    texture2d<float, access::sample> gibsGIArg [[texture(14), function_constant(kBoundMaterials)]], // GIBS indirect lighting
    texture2d<float, access::sample> texSSCSArg [[texture(15), function_constant(kBoundMaterials)]], // screen-space contact shadow
    const device SystemTexs* systemTexs [[buffer(14), function_constant(kBindlessMaterials)]],
    // RT reflection/refraction textures: direct args on the bound path,
    // resolved from systemTexs on the bindless (ICB) path — same split as the
    // system textures above. Resolved to locals at the top of the body.
    texture2d<float, access::sample> texReflectionArg [[texture(16), function_constant(kBoundMaterials)]], // RT mirror reflections
    texture2d<float, access::sample> texRefractionArg [[texture(17), function_constant(kBoundMaterials)]], // RT refractions (transmission)
    // Terrain detail-layer arrays (grass/rock/dirt/snow albedo + tangent-space
    // normal), world-space tiled — sampled only by the shaderModel == 1
    // (Terrain) branch, bound path only (the ICB/bindless system table has no
    // slots for them; terrain shades standard there). Metal twin of RHIMain.frag
    // set2 b13/b14.
    texture2d_array<float, access::sample> terrainDetailAlbedo [[texture(18), function_constant(kBoundMaterials)]],
    texture2d_array<float, access::sample> terrainDetailNormal [[texture(19), function_constant(kBoundMaterials)]],
    const device DirLight* directionalLights [[buffer(0)]],
    const device PointLight* pointLights [[buffer(1)]],
    const device Cluster* clusters [[buffer(2)]],
    constant CameraData& camera [[buffer(3)]],
    constant float2& screenSize [[buffer(4)]],
    constant packed_uint3& gridSize [[buffer(5)]],
    constant float& time [[buffer(6)]],
    const device RectLight* rectLights [[buffer(7)]],
    constant uint& rectLightCount [[buffer(8)]],
    constant PSSMData& pssmData [[buffer(9)]],
    constant uint& gibsEnabled [[buffer(10)]], // GIBS enable flag
    // Material fetched by id here, not passed through inter-stage (the 112-byte
    // MaterialData overflowed Metal's per-vertex output). buffer(19): 0-18 are
    // taken (11 = dirLightCount, 12 = mainDebugFlags, 13/14 = bindless tables,
    // 15/16 = spot, 17/18 = RT params), so materials lives past them.
    const device MaterialData* materials [[buffer(19)]],
    // Perf-isolation debug flags (bit0 = skip point-light loop, bit1 = skip
    // shadow). buffer(11) is dirLightCount (Vulkan-only, unread here), so this
    // takes buffer(12). Mirrors RHIMain.frag's mainDebugFlags.
    constant uint& mainDebugFlags [[buffer(12)]],
    // Spot lights at buffer(16): buffer(14) is the bindless systemTexs table,
    // so a plain buffer(14) here fails specialization ("invalid location").
    const device SpotLight* spotLights [[buffer(16)]],
    // x = spot light count, y = shadow-format flags (bit0 = the point-shadow
    // texture carries RGB channels: R point / G rect / B spot; 0 on legacy
    // R16F targets so rect/spot stay unshadowed instead of black).
    constant uint2& spotRectParams [[buffer(15)]],
    // RT reflection/refraction composite params (x = enabled, y = intensity).
    // Plain buffers at 17/18 — free on BOTH the bound and bindless paths (13/14
    // are the bindless argument tables, 16 is spotLights), so the RT composite
    // works in either draw mode. Buffers are legal on ICB fragments (only
    // direct texture/sampler args are rejected).
    constant float2& reflectionParams [[buffer(17)]],
    constant float2& refractionParams [[buffer(18)]]
) {
    constexpr sampler s(address::repeat, filter::linear, mip_filter::linear);

    MaterialData material = materials[in.materialID];

    // Resolve the material texture set once: bound slots (normal path) or the
    // bindless table entry for this fragment's material (ICB path). The dead
    // branch references a disabled argument, which is legal — it's eliminated
    // when the function constant is folded at pipeline build.
    texture2d<float, access::sample> matAlbedo    = kBindlessMaterials ? materialTexs[in.materialID].albedo    : texAlbedo;
    texture2d<float, access::sample> matNormal    = kBindlessMaterials ? materialTexs[in.materialID].normal    : texNormal;
    texture2d<float, access::sample> matMetallic  = kBindlessMaterials ? materialTexs[in.materialID].metallic  : texMetallic;
    texture2d<float, access::sample> matRoughness = kBindlessMaterials ? materialTexs[in.materialID].roughness : texRoughness;
    texture2d<float, access::sample> matOcclusion = kBindlessMaterials ? materialTexs[in.materialID].occlusion : texOcclusion;
    texture2d<float, access::sample> matEmissive  = kBindlessMaterials ? materialTexs[in.materialID].emissive  : texEmissive;

    // System textures under their original names — the rest of the body (and
    // its lambdas/helper calls) is identical on both paths.
    texture2d<float, access::sample>     texAO          = kBindlessMaterials ? systemTexs->texAO          : texAOArg;
    texture2d<float, access::sample>     texShadow      = kBindlessMaterials ? systemTexs->texShadow      : texShadowArg;
    texturecube<float, access::sample>   irradianceMap  = kBindlessMaterials ? systemTexs->irradianceMap  : irradianceMapArg;
    texturecube<float, access::sample>   prefilterMap   = kBindlessMaterials ? systemTexs->prefilterMap   : prefilterMapArg;
    texture2d<float, access::sample>     brdfLUT        = kBindlessMaterials ? systemTexs->brdfLUT        : brdfLUTArg;
    texture2d<float, access::sample>     rectLightVideo = kBindlessMaterials ? systemTexs->rectLightVideo : rectLightVideoArg;
    depth2d_array<float, access::sample> pssmShadowMaps = kBindlessMaterials ? systemTexs->pssmShadowMaps : pssmShadowMapsArg;
    texture2d<float, access::sample>     texPointShadow = kBindlessMaterials ? systemTexs->texPointShadow : texPointShadowArg;
    texture2d<float, access::sample>     gibsGI         = kBindlessMaterials ? systemTexs->gibsGI         : gibsGIArg;
    texture2d<float, access::sample>     texSSCS        = kBindlessMaterials ? systemTexs->texSSCS        : texSSCSArg;
    texture2d<float, access::sample>     texReflection  = kBindlessMaterials ? systemTexs->texReflection  : texReflectionArg;
    texture2d<float, access::sample>     texRefraction  = kBindlessMaterials ? systemTexs->texRefraction  : texRefractionArg;

    // Prototype UV: triplanar mapping with world space or object space
    // Mode: 0 = Off, 1 = World Space (static objects), 2 = Object Space (dynamic objects)
    if (material.prototypeUVMode > 0.5) {
        float3 pos;
        float3 n;
        if (material.prototypeUVMode > 1.5) {
            // Object Space: position and normal in local space (texture follows object rotation)
            pos = in.scaledLocalPos;
            n = abs(normalize(in.localNormal));
        } else {
            // World Space: position and normal in world space (texture fixed in world)
            pos = in.worldPosition.xyz;
            n = abs(normalize(in.worldNormal.xyz));
        }

        // Select projection plane based on dominant normal axis
        if (n.x > n.y && n.x > n.z) {
            in.uv = pos.yz * material.uvScale;
        } else if (n.y > n.z) {
            in.uv = pos.xz * material.uvScale;
        } else {
            in.uv = pos.xy * material.uvScale;
        }
    }

    float4 baseColor = matAlbedo.sample(s, in.uv);
    // glTF MASK cutout: per-material cutoff (0 = disabled for OPAQUE/BLEND).
    if (material.emissiveFactor.a > 0.0 && baseColor.a * material.baseColorFactor.a < material.emissiveFactor.a) {
        discard_fragment();
    }
    Surface surf;
    surf.color = srgbToLinear(baseColor.rgb * material.baseColorFactor.rgb);
    surf.ao = matOcclusion.sample(s, in.uv).r * material.occlusionStrength;
    surf.roughness = matRoughness.sample(s, in.uv).g * material.roughnessFactor;
    surf.metallic = matMetallic.sample(s, in.uv).b * material.metallicFactor;
    // Emissive is an sRGB-authored colour (like albedo) — linearize it before
    // it joins the linear lighting sum. (Was linearToSRGB, the wrong direction:
    // that re-encodes toward sRGB and brightens; the sRGB->linear encode
    // belongs only at the final output, which PostProcess/the swapchain do.)
    surf.emission = srgbToLinear(matEmissive.sample(s, in.uv).rgb * material.emissiveFactor.rgb) * material.emissiveStrength;
    surf.subsurface = material.subsurface;
    surf.specular = material.specular;
    surf.specular_tint = material.specularTint;
    surf.anisotropic = material.anisotropic;
    surf.sheen = material.sheen;
    surf.sheen_tint = material.sheenTint;
    surf.clearcoat = material.clearcoat;
    surf.clearcoat_gloss = material.clearcoatGloss;

    float3 N = normalize(float3(in.worldNormal));
    float3 T = normalize(float3(in.worldTangent));
    T = normalize(T - dot(T, N) * N);
    float3 B = normalize(cross(N, T) * in.worldTangent.w);
    float3x3 TBN = float3x3(T, B, N);
    float3 norm = normalize(TBN * normalize(matNormal.sample(s, in.uv).rgb * 2.0 - 1.0));

    // Terrain: replace albedo + normal with the world-space detail-layer splat
    // (mirrors RHIMain.frag's shaderModel == 1 branch). in.uv.x carries the
    // baked height01; the geometric normal drives slope. Terrain then shades
    // as a rough dielectric through the same lighting below. Bound path only:
    // the ICB/bindless table carries no detail arrays, so terrain falls back
    // to standard shading (palette-LUT albedo) there.
    if (kBoundMaterials && material.shaderModel == 1.0) {
        float3 tAlbedo, tN;
        // Height-field descriptor packed into the terrain material's spare fields
        // (see renderer.cpp material upload). Seed is carried as raw bits.
        float noiseFreq   = material.subsurface;
        float heightScale = material.specular;
        int   octaves     = int(material.specularTint + 0.5);
        uint  seed        = as_type<uint>(material.anisotropic);
        trgShadeTerrain(in.worldPosition.xyz, noiseFreq, octaves, seed, heightScale,
                        clamp(in.uv.x, 0.0, 1.0), terrainDetailAlbedo, terrainDetailNormal, tAlbedo, tN);
        surf.color = tAlbedo;  // detail albedo is already linearized in the blend
        surf.roughness = 0.95;
        surf.metallic = 0.0;
        norm = tN;
    }

    float3 viewDir = normalize(camera.position - in.worldPosition.xyz);

    float2 screenUV = in.position.xy / screenSize;

    // --- Shadow factor: RT shadow for near region, PSSM for mid/far ---
    constexpr sampler shadowCmpSampler(
        address::clamp_to_edge,
        filter::linear,
        compare_func::less_equal
    );
    constexpr float PSSM_TEXEL = 1.0 / 4096.0;
    constexpr float PSSM_BIAS  = 0.002;

    // abs(): view matrix is RH (visible z is negative); splits are positive distances
    float viewDepth = abs((camera.view * in.worldPosition).z);

    // Helper: sample PSSM shadow with configurable PCF
    auto samplePSSMShadow = [&](int cascadeIndex, float2 shadowUV, float refDepth) -> float {
        float pcf = 0.0;
        uint sampleCount = pssmData.pcfSampleCount;

        if (sampleCount <= 4) {
            // 4-tap Poisson disk
            for (int i = 0; i < 4; i++) {
                pcf += pssmShadowMaps.sample_compare(
                    shadowCmpSampler,
                    shadowUV + poissonDisk4[i] * PSSM_TEXEL * 2.0,
                    cascadeIndex, refDepth
                );
            }
            return pcf / 4.0;
        } else if (sampleCount <= 8) {
            // 8-tap Poisson disk
            for (int i = 0; i < 8; i++) {
                pcf += pssmShadowMaps.sample_compare(
                    shadowCmpSampler,
                    shadowUV + poissonDisk8[i] * PSSM_TEXEL * 2.0,
                    cascadeIndex, refDepth
                );
            }
            return pcf / 8.0;
        } else if (sampleCount <= 16) {
            // 16-tap Poisson disk
            for (int i = 0; i < 16; i++) {
                pcf += pssmShadowMaps.sample_compare(
                    shadowCmpSampler,
                    shadowUV + poissonDisk16[i] * PSSM_TEXEL * 2.0,
                    cascadeIndex, refDepth
                );
            }
            return pcf / 16.0;
        } else {
            // 32-tap: 16-tap Poisson + 16-tap rotated
            for (int i = 0; i < 16; i++) {
                pcf += pssmShadowMaps.sample_compare(
                    shadowCmpSampler,
                    shadowUV + poissonDisk16[i] * PSSM_TEXEL * 2.0,
                    cascadeIndex, refDepth
                );
                // Rotated samples for better coverage
                float2 rotated = float2(
                    poissonDisk16[i].x * 0.7071 - poissonDisk16[i].y * 0.7071,
                    poissonDisk16[i].x * 0.7071 + poissonDisk16[i].y * 0.7071
                ) * 1.5;
                pcf += pssmShadowMaps.sample_compare(
                    shadowCmpSampler,
                    shadowUV + rotated * PSSM_TEXEL * 2.0,
                    cascadeIndex, refDepth
                );
            }
            return pcf / 32.0;
        }
    };

    // Direction toward the sun; used for the slope-scaled shadow bias.
    float3 shadowL = normalize(-directionalLights[0].direction);

    // Helper: sample a specific cascade
    auto sampleCascade = [&](int ci) -> float {
        float4 lsPos = pssmData.lightSpaceMatrices[ci] * in.worldPosition;
        float3 proj  = lsPos.xyz / lsPos.w;
        float2 shadowUV = float2(proj.x * 0.5 + 0.5, 0.5 - proj.y * 0.5);
        // Per-cascade + slope-scaled depth bias. A flat bias is fine for the
        // near cascade but far cascades cover far more world per texel, and
        // grazing surfaces (small N·L, e.g. a ceiling lit obliquely) self-shadow
        // — the moiré / "z-fighting" acne that also swallows lit regions. Scale
        // the bias with cascade index and inverse N·L to suppress both.
        float ndl = max(dot(N, shadowL), 0.0);
        float slope = clamp(1.0 - ndl, 0.0, 1.0);
        float bias = PSSM_BIAS * float(ci + 1) * (1.0 + 2.0 * slope);
        float refDepth = proj.z - bias;
        return samplePSSMShadow(ci, shadowUV, refDepth);
    };

    // Crisp, non-repeating fetch for the screen-space RT shadow. Reusing the
    // material sampler `s` (address::repeat, mip_filter::linear) can pull a
    // blurred mip / wrap at screen edges, softening the RT shadow right where it
    // has to line up with PSSM.
    constexpr sampler rtShadowSampler(
        address::clamp_to_edge,
        filter::linear,
        mip_filter::none
    );

    // RT↔PSSM cross-fade is centred on rtEnd instead of starting there. The RT
    // shadow has crisp, contact-accurate edges; PSSM is slightly offset
    // (peter-panning from the depth bias + widened ortho range). A one-sided
    // fade lets the accurate RT shadow end abruptly at rtEnd, exposing a lit
    // sliver where the offset PSSM edge has not caught up yet — the bright line.
    // Centring the window keeps RT dominant across the contact region and only
    // hands off to PSSM well past the seam.
    float rtEnd     = pssmData.cascadeSplits.x;
    float halfBlend = pssmData.blendRange * 0.5;
    float blendLo   = rtEnd - halfBlend;
    float blendHi   = rtEnd + halfBlend;

    float shadowFactor;
    int debugCascade = -1; // -1 = RT, 0-2 = PSSM cascades

    if (viewDepth < blendLo) {
        // Fully inside the RT region
        shadowFactor = texShadow.sample(rtShadowSampler, screenUV).r;
        debugCascade = -1;
    } else {
        // At or past the RT boundary: evaluate the PSSM cascade
        int ci = 0;
        if      (viewDepth > pssmData.cascadeSplits.z) ci = 2;
        else if (viewDepth > pssmData.cascadeSplits.y) ci = 1;
        debugCascade = ci;

        shadowFactor = sampleCascade(ci);

        // Cascade blend: smooth transition between cascades
        float cascadeBlend = pssmData.cascadeBlendRange;
        if (cascadeBlend > 0.0 && ci < 2) {
            float cascadeEnd = (ci == 0) ? pssmData.cascadeSplits.y : pssmData.cascadeSplits.z;
            float blendStart = cascadeEnd - cascadeBlend;
            if (viewDepth > blendStart && viewDepth < cascadeEnd) {
                float nextShadow = sampleCascade(ci + 1);
                float t = (viewDepth - blendStart) / cascadeBlend;
                shadowFactor = mix(shadowFactor, nextShadow, smoothstep(0.0, 1.0, t));
            }
        }

        // Symmetric RT↔PSSM cross-fade window [blendLo, blendHi] around rtEnd
        if (viewDepth < blendHi && pssmData.blendRange > 0.0) {
            float rtShadow = texShadow.sample(rtShadowSampler, screenUV).r;
            float t = (viewDepth - blendLo) / pssmData.blendRange; // 0 at blendLo → 1 at blendHi
            shadowFactor = mix(rtShadow, shadowFactor, smoothstep(0.0, 1.0, t));
            if (t < 0.5) debugCascade = -1; // RT-dominant half shows as RT in debug view
        }
    }

    // Debug visualization: show cascade colors
    if (pssmData.debugVisualize > 0) {
        float3 cascadeColors[4] = {
            float3(0.2, 0.8, 0.2), // RT = green
            float3(0.8, 0.2, 0.2), // Cascade 0 = red
            float3(0.2, 0.2, 0.8), // Cascade 1 = blue
            float3(0.8, 0.8, 0.2)  // Cascade 2 = yellow
        };
        float3 cascadeColor = cascadeColors[debugCascade + 1];
        return float4(cascadeColor * shadowFactor, 1.0);
    }

    // Debug bit1: drop the shadow term to isolate its cost.
    if ((mainDebugFlags & 2u) != 0u) shadowFactor = 1.0;

    // Screen-space contact shadows tighten the near contact the RT/PSSM shadow
    // misses. min() = shadowed if either says so (no double-darkening).
    shadowFactor = min(shadowFactor, texSSCS.sample(rtShadowSampler, screenUV).r);

    float3 result = float3(0.0);
    result += CalculateDirectionalLight(directionalLights[0], norm, T, B, viewDir, surf) * shadowFactor;

    uint tileX = uint(screenUV.x * float(gridSize.x));
    uint tileY = uint((1.0 - screenUV.y) * float(gridSize.y));
    // float depthVS = (camera.view * in.worldPosition).z;
    // uint tileZ = uint((log(abs(depthVS) / camera.near) * gridSize.z) / log(camera.far / camera.near));
    // uint clusterIndex = tileX + (tileY * gridSize.x) + (tileZ * gridSize.x * gridSize.y);
    // Cluster cluster = clusters[clusterIndex];
    // uint lightCount = cluster.lightCount;

    // // Debug output - view space depth
    // // return float4((-depthVS - camera.near) / (camera.far - camera.near), 0.0, 0.0, 1.0);

    // // Debug output - tile indices
    // // return float4(tileX / float(gridSize.x), tileY / float(gridSize.y), 0.5, 1.0);
    // // return float4(tileX / float(gridSize.x), tileY / float(gridSize.y), 0.0, 1.0) * (tileZ / float(gridSize.z));

    // // Debug output - tile z
    // // return float4(tileZ / float(gridSize.z), 0.0, 0.0, 1.0);

    // // Debug output - cluster index
    // // return float4(clusterIndex / float(gridSize.x * gridSize.y * gridSize.z), 0.0, 0.0, 1.0);

    // // Debug output - cluster AABB
    // // return float4(
    // //     (cluster.min.x < cluster.max.x ? 1.0 : 0.0),
    // //     (cluster.min.y < cluster.max.y ? 1.0 : 0.0),
    // //     (cluster.min.z < cluster.max.z ? 1.0 : 0.0),
    // //     1.0
    // // );

    // // Debug output - light coverage
    // // for (uint i = 0; i < lightCount; i++) {
    // //     return float4(
    // //         cluster.lightIndices[i] == 0 ? 1.0 : 0.0,
    // //         cluster.lightIndices[i] == 1 ? 1.0 : 0.0,
    // //         cluster.lightIndices[i] == 2 ? 1.0 : 0.0,
    // //         1.0
    // //     );
    // // }

    // // Debug output - light count
    // // return float4(lightCount / 100.0, 0.0, 0.0, 1.0);

    // for (uint i = 0; i < lightCount; i++) {
    //     uint lightIndex = cluster.lightIndices[i];
    //     result += CalculatePointLight(pointLights[lightIndex], norm, T, B, viewDir, surf, in.worldPosition.xyz);
    // }

    uint tileIndex = tileX + tileY * gridSize.x;
    // Reference, not copy: Cluster is ~1KB (lightIndices[256]); copying it per
    // fragment spills to stack and reads the whole struct from device memory.
    const device Cluster& tile = clusters[tileIndex];
    // Debug bit0: skip the whole tiled point-light loop to isolate its cost.
    if ((mainDebugFlags & 1u) == 0u) {
        uint lightCount = tile.lightCount;
        float pointShadow = texPointShadow.sample(s, screenUV).r;
        for (uint i = 0; i < lightCount; i++) {
            uint lightIndex = tile.lightIndices[i];
            result += CalculatePointLight(pointLights[lightIndex], norm, T, B, viewDir, surf, in.worldPosition.xyz) * pointShadow;
        }
    }

    // Rect area lights, shadowed by the stochastic pass's G channel when the
    // RGB shadow format is active (spotRectParams.y bit0); legacy R16F targets
    // read G as 0, so the flag keeps them fully lit there instead of black.
    {
        float rectShadow = (spotRectParams.y & 1u) ? texPointShadow.sample(s, screenUV).g : 1.0;
        for (uint i = 0; i < rectLightCount; i++) {
            result += CalculateRectLight(rectLights[i], norm, in.worldPosition.xyz, viewDir, surf, rectLightVideo) * rectShadow;
        }
    }

    // Spot lights (loop-all; typical scenes carry a handful, so no clustering).
    // Shadowed by the stochastic pass's B channel under the same flag.
    {
        float spotShadow = (spotRectParams.y & 1u) ? texPointShadow.sample(s, screenUV).b : 1.0;
        for (uint i = 0; i < spotRectParams.x; i++) {
            result += CalculateSpotLight(spotLights[i], norm, T, B, viewDir, surf, in.worldPosition.xyz) * spotShadow;
        }
    }

    // Screen-space AO attenuates ambient/indirect light only — multiplying
    // direct light by AO is physically wrong and dirties lit surfaces
    float screenAO = texAO.sample(s, screenUV).r;

    // GIBS Global Illumination or IBL fallback
    if (gibsEnabled > 0) {
        // Sample GIBS indirect lighting at screen position
        float3 giContribution = gibsGI.sample(s, screenUV).rgb;
        // Apply ambient occlusion to indirect lighting
        result += giContribution * surf.ao * screenAO;
    } else if (material.iblEnabled > 0.5) {
        result += CalculateIBL(norm, viewDir, surf, irradianceMap, prefilterMap, brdfLUT) * screenAO;
    } else {
        result += float3(0.03) * surf.ao * surf.color * screenAO; // minimal ambient fallback
    }

    // RT mirror reflections (half-res, bilinearly upsampled by the sample).
    // Fresnel-weighted like the IBL specular, faded by roughness — the traced
    // ray is a mirror ray, so rough surfaces should not show sharp reflections.
    if (reflectionParams.x > 0.5) {
        float3 refl = texReflection.sample(s, screenUV).rgb;
        float NdotV = max(dot(norm, viewDir), 0.0);
        float3 F0r = mix(float3(0.04), surf.color, surf.metallic);
        float3 Fr = FresnelSchlickRoughness(NdotV, F0r, surf.roughness);
        float roughFade = (1.0 - surf.roughness) * (1.0 - surf.roughness);
        result += refl * Fr * roughFade * reflectionParams.y * screenAO;
    }

    // RT refractions (KHR_materials_transmission): blend the traced
    // transmitted radiance in by the material's transmission factor. What
    // Fresnel reflects cannot transmit (1 - F), the base color tints the
    // transmitted light like glTF's BTDF, and the sharp traced ray fades by
    // roughness exactly like the mirror reflection above. mix() REPLACES the
    // accumulated diffuse/GI response instead of adding — a transmissive
    // surface trades its diffuse lobe for transmission per the spec.
    if (refractionParams.x > 0.5 && material.transmission > 0.0) {
        float3 refr = texRefraction.sample(s, screenUV).rgb;
        float NdotV = max(dot(norm, viewDir), 0.0);
        float3 F0t = mix(float3(0.04), surf.color, surf.metallic);
        float3 Ft = FresnelSchlickRoughness(NdotV, F0t, surf.roughness);
        float roughFade = (1.0 - surf.roughness) * (1.0 - surf.roughness);
        float3 transmitted = refr * surf.color * (1.0 - Ft) * refractionParams.y;
        result = mix(result, transmitted, material.transmission * roughFade);
    }

    result += surf.emission;

    return float4(result, 1.0);
}