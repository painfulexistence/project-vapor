#pragma once
#include <metal_stdlib>
using namespace metal;
// Shared PBR shading library (MSL). Pure BRDF / analytic-light / IBL helpers,
// extracted verbatim from 3d_pbr_normal_mapped.metal so the forward PBR path
// and the meshlet path shade through the SAME math (no divergence). Depends on
// the types + constants in 3d_common.metal (MaterialData, DirLight, PointLight,
// SpotLight, RectLight, Cluster, PSSMData, PI, srgbToLinear, ...): the includer
// MUST include 3d_common.metal BEFORE this header. No entry points here, so the
// asset pipeline's shader validator skips it (include-only).

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

// Split IBL: returns the diffuse (irradiance) and specular (prefiltered env)
// terms separately so the forward composite can let RT reflection REPLACE the
// specular term instead of double-counting it against the prefilter. Same math
// as the combined overload above.
void CalculateIBL(
    float3 norm,
    float3 viewDir,
    Surface surf,
    texturecube<float, access::sample> irradianceMap,
    texturecube<float, access::sample> prefilterMap,
    texture2d<float, access::sample> brdfLUT,
    thread float3& outDiffuse,
    thread float3& outSpecular
) {
    constexpr sampler cubeSampler(filter::linear, mip_filter::linear);
    constexpr sampler lutSampler(filter::linear, address::clamp_to_edge);

    float NdotV = max(dot(norm, viewDir), 0.0);

    float3 F0 = mix(float3(0.04), surf.color, surf.metallic);
    float3 F = FresnelSchlickRoughness(NdotV, F0, surf.roughness);
    float3 kD = (1.0 - F) * (1.0 - surf.metallic);

    float3 irradiance = irradianceMap.sample(cubeSampler, norm).rgb;
    float3 diffuseIBL = irradiance * surf.color * kD;

    float3 R = reflect(-viewDir, norm);
    const float MAX_REFLECTION_LOD = 4.0;
    float3 prefilteredColor = prefilterMap.sample(cubeSampler, R, level(surf.roughness * MAX_REFLECTION_LOD)).rgb;
    float2 brdf = brdfLUT.sample(lutSampler, float2(NdotV, surf.roughness)).rg;
    float3 specularIBL = prefilteredColor * (F * brdf.x + brdf.y);

    outDiffuse  = diffuseIBL  * surf.ao;
    outSpecular = specularIBL * surf.ao;
}

