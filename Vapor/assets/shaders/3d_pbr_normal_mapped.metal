#include <metal_stdlib>
using namespace metal;
#include "assets/shaders/3d_common.metal" // TODO: use more robust include path
#include "assets/shaders/3d_shadow_common.metal"


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
    float vh = max(dot(viewDir, halfway), 0.0);
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

    return CookTorranceBRDF(norm, tangent, bitangent, lightDir, viewDir, surf) * radiance * clamp(dot(norm, lightDir), 0.0, 1.0);
}

float3 CalculatePointLight(PointLight light, float3 norm, float3 tangent, float3 bitangent, float3 viewDir, Surface surf, float3 fragPos) {
    float3 lightDir = normalize(light.position - fragPos);
    float dist = distance(light.position, fragPos);
    float attenuation = 1.0 / (dist * dist);
    attenuation *= 1.0 - smoothstep(light.radius * 0.8, light.radius, dist);
    float3 radiance = attenuation * light.color * light.intensity;

    return CookTorranceBRDF(norm, tangent, bitangent, lightDir, viewDir, surf) * radiance * clamp(dot(norm, lightDir), 0.0, 1.0);
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
    constant uint& instanceID [[buffer(4)]]
) {
    RasterizerData vert;
    uint actualVertexID = instances[instanceID].vertexOffset + vertexID;
    float4x4 model = instances[instanceID].model;
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
    vert.material = materials[instances[instanceID].materialID];
    
    // Pass scaled local position and local normal for Object Space Triplanar
    float3 scale = float3(length(model[0].xyz), length(model[1].xyz), length(model[2].xyz));
    vert.scaledLocalPos = float3(in[actualVertexID].position) * scale;
    vert.localNormal = float3(in[actualVertexID].normal);
    
    return vert;
}

fragment float4 fragmentMain(
    RasterizerData in [[stage_in]],
    texture2d<float, access::sample> texAlbedo [[texture(0)]],
    texture2d<float, access::sample> texNormal [[texture(1)]],
    texture2d<float, access::sample> texMetallic [[texture(2)]],
    texture2d<float, access::sample> texRoughness [[texture(3)]],
    texture2d<float, access::sample> texOcclusion [[texture(4)]],
    texture2d<float, access::sample> texEmissive [[texture(5)]],
    texture2d<float, access::sample> texShadow [[texture(7)]],
    texturecube<float, access::sample> irradianceMap [[texture(8)]],
    texturecube<float, access::sample> prefilterMap [[texture(9)]],
    texture2d<float, access::sample> brdfLUT [[texture(10)]],
    depth2d_array<float, access::sample> csmShadowMap [[texture(11)]],
    const device DirLight* directionalLights [[buffer(0)]],
    const device PointLight* pointLights [[buffer(1)]],
    const device Cluster* clusters [[buffer(2)]],
    constant CameraData& camera [[buffer(3)]],
    constant float2& screenSize [[buffer(4)]],
    constant packed_uint3& gridSize [[buffer(5)]],
    constant float& time [[buffer(6)]],
    constant CascadeShadowData& csmData [[buffer(7)]],
    constant HybridShadowData& hybridData [[buffer(8)]],
    constant uint& shadowMode [[buffer(9)]]
) {
    constexpr sampler s(address::repeat, filter::linear, mip_filter::linear);

    MaterialData material = in.material;

    if (material.usePrototypeUV > 0.5) {
        float3 n = abs(normalize(in.localNormal));
        if (n.x > n.y && n.x > n.z) {
            in.uv = in.scaledLocalPos.yz;
        } else if (n.y > n.z) {
            in.uv = in.scaledLocalPos.xz;
        } else {
            in.uv = in.scaledLocalPos.xy;
        }
    }

    float4 baseColor = texAlbedo.sample(s, in.uv);
    if (baseColor.a * material.baseColorFactor.a < 0.5) {
        discard_fragment();
    }
    Surface surf;
    surf.color = srgbToLinear(baseColor.rgb * material.baseColorFactor.rgb);
    surf.ao = texOcclusion.sample(s, in.uv).r * material.occlusionStrength;
    surf.roughness = texRoughness.sample(s, in.uv).g * material.roughnessFactor;
    surf.metallic = texMetallic.sample(s, in.uv).b * material.metallicFactor;
    surf.emission = linearToSRGB(texEmissive.sample(s, in.uv).rgb * material.emissiveFactor) * material.emissiveStrength;
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
    float3 norm = normalize(TBN * normalize(texNormal.sample(s, in.uv).rgb * 2.0 - 1.0));
    float3 viewDir = normalize(camera.position - in.worldPosition.xyz);

    float2 screenUV = in.position.xy / screenSize;

    // Calculate view-space depth for cascade selection
    float4 viewPos = camera.view * in.worldPosition;
    float viewDepth = -viewPos.z;

    // Shadow sampler for CSM
    constexpr sampler shadowSampler(
        address::clamp_to_edge,
        filter::linear,
        compare_func::less
    );

    float3 result = float3(0.0);

    // Calculate shadow factor based on shadow mode
    // shadowMode: 0 = None, 1 = RayTraced, 2 = CSM, 3 = Hybrid
    float shadowFactor = 1.0;
    if (shadowMode == 1) {
        // Ray-traced shadow (from pre-computed shadow texture)
        shadowFactor = texShadow.sample(s, screenUV).r;
    } else if (shadowMode == 2) {
        // Cascaded Shadow Map
        shadowFactor = calculateCSMShadow(
            in.worldPosition.xyz,
            norm,
            viewDepth,
            in.position.xy,
            csmData,
            csmShadowMap,
            shadowSampler
        );

        // Debug: visualize cascades
        if (csmData.cascadeVisualization > 0) {
            uint cascade = selectCascade(viewDepth, csmData.cascadeSplits, csmData.cascadeCount);
            float3 cascadeColor = getCascadeDebugColor(cascade);
            return float4(mix(cascadeColor, float3(shadowFactor), 0.7), 1.0);
        }
    } else if (shadowMode == 3) {
        // Hybrid: CSM base + RT refinement in penumbra regions
        float csmShadow = calculateCSMShadow(
            in.worldPosition.xyz,
            norm,
            viewDepth,
            in.position.xy,
            csmData,
            csmShadowMap,
            shadowSampler
        );

        // Check if we're in a penumbra region (partial shadow)
        if (hybridData.hybridEnabled > 0 &&
            csmShadow > hybridData.penumbraThresholdLow &&
            csmShadow < hybridData.penumbraThresholdHigh) {
            // In penumbra region - use RT shadow for refinement
            float rtShadow = texShadow.sample(s, screenUV).r;
            // Blend CSM with RT based on penumbra region
            float penumbraFactor = smoothstep(
                hybridData.penumbraThresholdLow,
                hybridData.penumbraThresholdHigh,
                csmShadow
            );
            shadowFactor = mix(csmShadow, rtShadow, 0.5);
        } else {
            shadowFactor = csmShadow;
        }
    }

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
    Cluster tile = clusters[tileIndex];
    uint lightCount = tile.lightCount;
    for (uint i = 0; i < lightCount; i++) {
        uint lightIndex = tile.lightIndices[i];
        result += CalculatePointLight(pointLights[lightIndex], norm, T, B, viewDir, surf, in.worldPosition.xyz);
    }

    result += float3(0.2) * surf.ao * surf.color;
    // TODO: IBL
    // result += CalculateIBL(norm, viewDir, surf, irradianceMap, prefilterMap, brdfLUT);

    result += surf.emission;

    return float4(result, 1.0);
}