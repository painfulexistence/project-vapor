#include <metal_stdlib>
using namespace metal;
#include "Res/shaders/3d_common.metal"

// ──────────────────────────────────────────────────────────────────────────────
// Iridescent PBR — standard Disney PBR + thin-film interference (electroplating)
//
// Material parameter mapping (reuses existing MaterialData fields):
//   clearcoat          → iridescence strength  (0 = off, 1 = full iridescence)
//   clearcoatGloss     → film thickness factor  (0 = 300 nm, 1 = 700 nm)
// ──────────────────────────────────────────────────────────────────────────────

struct RasterizerData {
    float4 position [[position]];
    float2 uv;
    float4 worldPosition;
    float4 worldNormal;
    float4 worldTangent;
    float3 scaledLocalPos;
    float3 localNormal;
    MaterialData material;
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

// ── Shared BRDF helpers ───────────────────────────────────────────────────────

float GTR1_ir(float nh, float a) {
    if (a >= 1.0) return 1.0 / PI;
    float a2 = a * a;
    float t = 1.0 + (a2 - 1.0) * nh * nh;
    return (a2 - 1.0) / (PI * log(a2) * t);
}

float GTR2_aniso_ir(float nh, float hx, float hy, float ax, float ay) {
    float t = (hx * hx) / (ax * ax) + (hy * hy) / (ay * ay) + nh * nh;
    return 1.0 / (PI * ax * ay * t * t);
}

float SmithGGX_ir(float u, float r) {
    float a = r * r, b = u * u;
    return 1.0 / (u + sqrt(a + b - a * b));
}

float SmithGGX_aniso_ir(float u, float vx, float vy, float ax, float ay) {
    float t = vx * vx * ax * ax + vy * vy * ay * ay + u * u;
    return 1.0 / (u + sqrt(t));
}

float FresnelApprox_ir(float u) {
    return pow(1.0 + 0.0001 - u, 5.0);
}

float luminance_ir(float3 c) { return dot(c, float3(0.3, 0.6, 0.1)); }

// ── Thin-film interference ────────────────────────────────────────────────────
// Returns per-channel (RGB) reflectance from a thin dielectric film.
// wavelengths_nm: approximate center wavelengths for R, G, B channels.
// filmThickness_nm: optical thickness of the coating.
// filmIOR: refractive index of the film (typical metal plating ~1.8-2.5).
float3 ThinFilmReflectance(float NdotV, float filmThickness_nm, float filmIOR) {
    const float3 lambda = float3(700.0, 546.0, 436.0); // R G B wavelengths in nm

    // Refracted angle inside the film via Snell's law
    float sinT2 = (1.0 - NdotV * NdotV) / (filmIOR * filmIOR);
    float cosT  = sqrt(max(0.0, 1.0 - sinT2));

    // Optical path difference (two-beam approximation)
    float opd = 2.0 * filmIOR * filmThickness_nm * cosT;

    // Phase per wavelength; +π phase shift at the denser medium boundary
    float3 phi = (2.0 * PI * opd) / lambda + PI;

    // Simplified two-beam interference: I = (1 + cos(phi)) / 2
    return saturate((1.0 + cos(phi)) * 0.5);
}

// ── Cook-Torrance BRDF with iridescent Fresnel ────────────────────────────────
float3 IridescentBRDF(
    float3 norm, float3 tangent, float3 bitangent,
    float3 lightDir, float3 viewDir,
    Surface surf,
    float NdotV,
    float3 iridF          // pre-computed iridescent Fresnel color
) {
    float3 halfway = normalize(lightDir + viewDir);
    float nv = max(NdotV, 0.0001);
    float nl = max(dot(norm, lightDir), 0.0);
    float nh = max(dot(norm, halfway), 0.0);
    float vh = max(dot(viewDir, halfway), 0.0);
    float lh = max(dot(lightDir, halfway), 0.0);

    float lum  = luminance_ir(surf.color);
    float3 tint = lum > 0.0 ? surf.color / lum : float3(1.0);

    float fh = FresnelApprox_ir(lh);
    float fl = FresnelApprox_ir(nl);
    float fv = FresnelApprox_ir(nv);

    // Diffuse (Burley)
    float fd90 = 0.5 + 2.0 * lh * lh * surf.roughness;
    float kd   = mix(1.0, fd90, fl) * mix(1.0, fd90, fv);

    // Specular – anisotropic GGX
    float aspect = sqrt(1.0 - surf.anisotropic * 0.9);
    float ax = max(0.001, surf.roughness * surf.roughness / aspect);
    float ay = max(0.001, surf.roughness * surf.roughness * aspect);
    float hx = dot(halfway, tangent), hy = dot(halfway, bitangent);
    float lx = dot(lightDir, tangent), ly = dot(lightDir, bitangent);
    float vx = dot(viewDir, tangent),  vy = dot(viewDir, bitangent);

    float D = GTR2_aniso_ir(nh, hx, hy, ax, ay);
    float G = SmithGGX_aniso_ir(nl, lx, ly, ax, ay) * SmithGGX_aniso_ir(nv, vx, vy, ax, ay);

    // Base specular F0
    float3 spec0 = mix(
        surf.specular * 0.08 * mix(float3(1.0), tint, surf.specular_tint),
        surf.color,
        surf.metallic
    );

    // Blend standard Fresnel with iridescent Fresnel
    float iridStrength = surf.clearcoat; // reused field
    float3 F_base    = mix(spec0, float3(1.0), fh);
    float3 F_irid    = iridF;
    float3 F         = mix(F_base, F_irid, iridStrength);

    float3 specular = D * G * F;

    // Sheen
    float3 sheen = fh * surf.sheen * mix(float3(1.0), tint, surf.sheen_tint);

    return ((kd * surf.color / PI + sheen) * (1.0 - surf.metallic) + specular) * nl;
}

float3 CalculateDirectionalLightIrid(DirLight light, float3 norm, float3 T, float3 B,
                                      float3 viewDir, Surface surf, float NdotV, float3 iridF) {
    float3 lightDir = normalize(-light.direction);
    float3 radiance = light.color * light.intensity;
    // IridescentBRDF already multiplies by nl (N·L), so no extra NdotL factor here
    return IridescentBRDF(norm, T, B, lightDir, viewDir, surf, NdotV, iridF) * radiance;
}

float3 CalculatePointLightIrid(PointLight light, float3 norm, float3 T, float3 B,
                                float3 viewDir, Surface surf, float3 fragPos, float NdotV, float3 iridF) {
    float3 lightDir    = normalize(light.position - fragPos);
    float  dist        = distance(light.position, fragPos);
    float  attenuation = 1.0 / (dist * dist);
    attenuation       *= 1.0 - smoothstep(light.radius * 0.8, light.radius, dist);
    float3 radiance    = attenuation * light.color * light.intensity;
    return IridescentBRDF(norm, T, B, lightDir, viewDir, surf, NdotV, iridF) * radiance;
}

// Fresnel-Schlick with roughness (for IBL)
float3 FresnelSchlickRoughness_ir(float cosTheta, float3 F0, float roughness) {
    return F0 + (max(float3(1.0 - roughness), F0) - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

float3 CalculateIBL_ir(
    float3 norm, float3 viewDir, Surface surf, float3 iridF,
    texturecube<float, access::sample> irradianceMap,
    texturecube<float, access::sample> prefilterMap,
    texture2d<float, access::sample>   brdfLUT
) {
    constexpr sampler cubeSampler(filter::linear, mip_filter::linear);
    constexpr sampler lutSampler(filter::linear, address::clamp_to_edge);

    float NdotV = max(dot(norm, viewDir), 0.0);
    float3 F0   = mix(float3(0.04), surf.color, surf.metallic);

    float iridStrength = surf.clearcoat;
    float3 F_base = FresnelSchlickRoughness_ir(NdotV, F0, surf.roughness);
    float3 F      = mix(F_base, iridF, iridStrength);

    float3 kS = F;
    float3 kD = (1.0 - kS) * (1.0 - surf.metallic);

    float3 irradiance    = irradianceMap.sample(cubeSampler, norm).rgb;
    float3 diffuseIBL    = irradiance * surf.color * kD;

    float3 R                = reflect(-viewDir, norm);
    float  mipLevel         = surf.roughness * 4.0;
    float3 prefilteredColor = prefilterMap.sample(cubeSampler, R, level(mipLevel)).rgb;
    float2 brdf             = brdfLUT.sample(lutSampler, float2(NdotV, surf.roughness)).rg;
    float3 specularIBL      = prefilteredColor * (F * brdf.x + brdf.y);

    return (diffuseIBL + specularIBL) * surf.ao;
}

// ── Vertex stage (identical to 3d_pbr_normal_mapped) ─────────────────────────

vertex RasterizerData vertexMain(
    uint vertexID [[vertex_id]],
    constant CameraData&    camera    [[buffer(0)]],
    constant MaterialData*  materials [[buffer(1)]],
    constant InstanceData*  instances [[buffer(2)]],
    device const VertexData* in       [[buffer(3)]],
    constant uint& instanceID         [[buffer(4)]]
) {
    RasterizerData vert;
    uint actual    = instances[instanceID].vertexOffset + vertexID;
    float4x4 model = instances[instanceID].model;
    float3x3 N     = transpose(inverse(float3x3(model[0].xyz, model[1].xyz, model[2].xyz)));
    vert.worldNormal   = float4(N * float3(in[actual].normal), 0.0);
    vert.worldTangent  = float4(N * in[actual].tangent.xyz, in[actual].tangent.w);
    vert.worldPosition = model * float4(in[actual].position, 1.0);
    vert.position      = camera.proj * camera.view * vert.worldPosition;
    vert.uv            = in[actual].uv;
    vert.material      = materials[instances[instanceID].materialID];
    float3 scale       = float3(length(model[0].xyz), length(model[1].xyz), length(model[2].xyz));
    vert.scaledLocalPos = float3(in[actual].position) * scale;
    vert.localNormal    = float3(in[actual].normal);
    return vert;
}

// ── Fragment stage ────────────────────────────────────────────────────────────

fragment float4 fragmentMain(
    RasterizerData in [[stage_in]],
    texture2d<float, access::sample> texAlbedo    [[texture(0)]],
    texture2d<float, access::sample> texNormal    [[texture(1)]],
    texture2d<float, access::sample> texMetallic  [[texture(2)]],
    texture2d<float, access::sample> texRoughness [[texture(3)]],
    texture2d<float, access::sample> texOcclusion [[texture(4)]],
    texture2d<float, access::sample> texEmissive  [[texture(5)]],
    texture2d<float, access::sample> texShadow    [[texture(7)]],
    texturecube<float, access::sample> irradianceMap [[texture(8)]],
    texturecube<float, access::sample> prefilterMap  [[texture(9)]],
    texture2d<float, access::sample>   brdfLUT       [[texture(10)]],
    const device DirLight*   directionalLights [[buffer(0)]],
    const device PointLight* pointLights       [[buffer(1)]],
    const device Cluster*    clusters          [[buffer(2)]],
    constant CameraData&     camera            [[buffer(3)]],
    constant float2&         screenSize        [[buffer(4)]],
    constant packed_uint3&   gridSize          [[buffer(5)]],
    constant float&          time              [[buffer(6)]]
) {
    constexpr sampler s(address::repeat, filter::linear, mip_filter::linear);

    MaterialData material = in.material;

    // Triplanar UV (same as PBR shader)
    if (material.prototypeUVMode > 0.5) {
        float3 pos, n;
        if (material.prototypeUVMode > 1.5) {
            pos = in.scaledLocalPos;
            n   = abs(normalize(in.localNormal));
        } else {
            pos = in.worldPosition.xyz;
            n   = abs(normalize(in.worldNormal.xyz));
        }
        if (n.x > n.y && n.x > n.z)      in.uv = pos.yz * material.uvScale;
        else if (n.y > n.z)               in.uv = pos.xz * material.uvScale;
        else                              in.uv = pos.xy * material.uvScale;
    }

    float4 baseColor = texAlbedo.sample(s, in.uv);
    if (baseColor.a * material.baseColorFactor.a < 0.5) discard_fragment();

    Surface surf;
    surf.color         = srgbToLinear(baseColor.rgb * material.baseColorFactor.rgb);
    surf.ao            = texOcclusion.sample(s, in.uv).r * material.occlusionStrength;
    surf.roughness     = texRoughness.sample(s, in.uv).g * material.roughnessFactor;
    surf.metallic      = texMetallic.sample(s, in.uv).b  * material.metallicFactor;
    surf.emission      = linearToSRGB(texEmissive.sample(s, in.uv).rgb * material.emissiveFactor)
                         * material.emissiveStrength;
    surf.subsurface    = material.subsurface;
    surf.specular      = material.specular;
    surf.specular_tint = material.specularTint;
    surf.anisotropic   = material.anisotropic;
    surf.sheen         = material.sheen;
    surf.sheen_tint    = material.sheenTint;
    surf.clearcoat     = material.clearcoat;      // iridescence strength
    surf.clearcoat_gloss = material.clearcoatGloss; // film thickness factor

    // TBN
    float3 N = normalize(float3(in.worldNormal));
    float3 T = normalize(float3(in.worldTangent));
    T = normalize(T - dot(T, N) * N);
    float3 B    = normalize(cross(N, T) * in.worldTangent.w);
    float3x3 TBN = float3x3(T, B, N);
    float3 norm  = normalize(TBN * normalize(texNormal.sample(s, in.uv).rgb * 2.0 - 1.0));
    float3 viewDir = normalize(camera.position - in.worldPosition.xyz);
    float  NdotV   = max(dot(norm, viewDir), 0.0001);

    // Thin-film iridescence: map clearcoatGloss [0,1] → film thickness [300, 700] nm
    float filmThickness = mix(300.0, 700.0, surf.clearcoat_gloss);
    float filmIOR       = 1.9; // typical titanium nitride / chrome plating
    float3 iridF = ThinFilmReflectance(NdotV, filmThickness, filmIOR);

    float2 screenUV   = in.position.xy / screenSize;
    float shadowFactor = texShadow.sample(s, screenUV).r;

    float3 result = float3(0.0);
    result += CalculateDirectionalLightIrid(
        directionalLights[0], norm, T, B, viewDir, surf, NdotV, iridF) * shadowFactor;

    uint tileX = uint(screenUV.x * float(gridSize.x));
    uint tileY = uint((1.0 - screenUV.y) * float(gridSize.y));
    uint tileIndex = tileX + tileY * gridSize.x;
    // Reference, not copy: Cluster is ~1KB (lightIndices[256]); copying it per
    // fragment spills to stack and reads the whole struct from device memory.
    const device Cluster& tile = clusters[tileIndex];
    for (uint i = 0; i < tile.lightCount; i++) {
        uint li = tile.lightIndices[i];
        result += CalculatePointLightIrid(
            pointLights[li], norm, T, B, viewDir, surf, in.worldPosition.xyz, NdotV, iridF);
    }

    // IBL
    if (material.iblEnabled > 0.5) {
        result += CalculateIBL_ir(norm, viewDir, surf, iridF,
                                   irradianceMap, prefilterMap, brdfLUT);
    } else {
        result += float3(0.03) * surf.ao * surf.color;
    }

    result += surf.emission;

    return float4(result, 1.0);
}
