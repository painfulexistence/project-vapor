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
};

struct RasterizerData {
    float4 position [[position]];
    float2 uv;
    float4 worldPosition;
    float4 worldNormal;
    float4 worldTangent;
    float3 scaledLocalPos;
    float3 localNormal;
    MaterialData material;
    uint materialID [[flat]];  // bindless table index (ICB mode)
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
    vert.material = materials[instances[iid].materialID];
    vert.materialID = instances[iid].materialID;
    
    // Pass scaled local position and local normal for Object Space Triplanar
    float3 scale = float3(length(model[0].xyz), length(model[1].xyz), length(model[2].xyz));
    vert.scaledLocalPos = float3(in[actualVertexID].position) * scale;
    vert.localNormal = float3(in[actualVertexID].normal);
    
    return vert;
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
    // Perf-isolation debug flags (bit0 = skip point-light loop, bit1 = skip
    // shadow). buffer(11) is dirLightCount (Vulkan-only, unread here), so this
    // takes buffer(12). Mirrors RHIMain.frag's mainDebugFlags.
    constant uint& mainDebugFlags [[buffer(12)]],
    // buffer(13) is reserved for the RT PR's reflectionParams.
    // NOTE: buffer(14) is the bindless SystemTexs argument table (function-
    // constant gated, but its slot is claimed statically), so spot lights live
    // at buffer(16) — a plain buffer(14) here collides with it and fails Metal
    // shader specialization ("spotLights has invalid location").
    const device SpotLight* spotLights [[buffer(16)]],
    // x = spot light count, y = shadow-format flags (bit0 = the point-shadow
    // texture carries RGB channels: R point / G rect / B spot; 0 on legacy
    // R16F targets so rect/spot stay unshadowed instead of black).
    constant uint2& spotRectParams [[buffer(15)]]
) {
    constexpr sampler s(address::repeat, filter::linear, mip_filter::linear);

    MaterialData material = in.material;

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
    if (material.alphaCutoff > 0.0 && baseColor.a * material.baseColorFactor.a < material.alphaCutoff) {
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
    surf.emission = srgbToLinear(matEmissive.sample(s, in.uv).rgb * material.emissiveFactor) * material.emissiveStrength;
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

    result += surf.emission;

    return float4(result, 1.0);
}