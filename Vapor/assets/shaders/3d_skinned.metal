#include <metal_stdlib>
using namespace metal;
#include "assets/shaders/3d_common.metal"

// Maximum bones per skeleton (must match CPU-side constant)
constant uint MAX_BONES_PER_SKELETON = 256;

// SkinnedVertexData and SkinnedInstanceData are defined in 3d_common.metal

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

// BRDF functions (same as 3d_pbr_normal_mapped.metal)
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
    float fd90 = 0.5 + 2.0 * fss90;
    float kd = mix(1.0, fd90, fl) * mix(1.0, fd90, fv);
    float fss = mix(1.0, fss90, fl) * mix(1.0, fss90, fv);
    float ss = 1.25 * (fss * (1.0 / (nl + nv + 0.0001) - 0.5) + 0.5);
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
    float D = GTR2_aniso(nh, hx, hy, ax, ay);
    float G = SmithGGX_aniso(nl, lx, ly, ax, ay) * SmithGGX_aniso(nv, vx, vy, ax, ay);
    float3 F = mix(spec0, float3(1.0), fh);
    float3 specular = D * G * F;
    float3 sheen = fh * surf.sheen * mix(float3(1), tint, surf.sheen_tint);
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

// GPU Skinning: Transform vertex by bone matrices
float4x4 calculateSkinMatrix(
    uint4 jointIndices,
    float4 jointWeights,
    constant float4x4* boneMatrices,
    uint boneMatrixOffset
) {
    float4x4 skinMatrix = boneMatrices[boneMatrixOffset + jointIndices.x] * jointWeights.x;
    skinMatrix += boneMatrices[boneMatrixOffset + jointIndices.y] * jointWeights.y;
    skinMatrix += boneMatrices[boneMatrixOffset + jointIndices.z] * jointWeights.z;
    skinMatrix += boneMatrices[boneMatrixOffset + jointIndices.w] * jointWeights.w;
    return skinMatrix;
}

vertex RasterizerData vertexMain(
    uint vertexID [[vertex_id]],
    constant CameraData& camera [[buffer(0)]],
    constant MaterialData* materials [[buffer(1)]],
    constant SkinnedInstanceData* instances [[buffer(2)]],
    device const SkinnedVertexData* in [[buffer(3)]],
    constant uint& instanceID [[buffer(4)]],
    constant float4x4* boneMatrices [[buffer(5)]]
) {
    RasterizerData vert;

    uint actualVertexID = instances[instanceID].vertexOffset + vertexID;
    float4x4 model = instances[instanceID].model;
    uint boneMatrixOffset = instances[instanceID].boneMatrixOffset;

    // Get vertex data
    float3 position = float3(in[actualVertexID].position);
    float3 normal = float3(in[actualVertexID].normal);
    float4 tangent = float4(in[actualVertexID].tangent);
    uint4 jointIndices = uint4(in[actualVertexID].jointIndices);
    float4 jointWeights = float4(in[actualVertexID].jointWeights);

    // Normalize weights (in case they don't sum to 1.0)
    float weightSum = jointWeights.x + jointWeights.y + jointWeights.z + jointWeights.w;
    if (weightSum > 0.0) {
        jointWeights /= weightSum;
    }

    // Calculate skin matrix from bone influences
    float4x4 skinMatrix = calculateSkinMatrix(jointIndices, jointWeights, boneMatrices, boneMatrixOffset);

    // Apply skinning to position and normal
    float4 skinnedPosition = skinMatrix * float4(position, 1.0);
    float3 skinnedNormal = normalize((skinMatrix * float4(normal, 0.0)).xyz);
    float3 skinnedTangent = normalize((skinMatrix * float4(tangent.xyz, 0.0)).xyz);

    // Apply model transform
    float3x3 normalMatrix = transpose(inverse(float3x3(
        model[0].xyz,
        model[1].xyz,
        model[2].xyz
    )));

    vert.worldNormal = float4(normalMatrix * skinnedNormal, 0.0);
    vert.worldTangent = float4(normalMatrix * skinnedTangent, tangent.w);
    vert.worldPosition = model * skinnedPosition;
    vert.position = camera.proj * camera.view * vert.worldPosition;
    vert.uv = in[actualVertexID].uv;
    vert.material = materials[instances[instanceID].materialID];

    // Pass scaled local position and local normal for Object Space Triplanar
    float3 scale = float3(length(model[0].xyz), length(model[1].xyz), length(model[2].xyz));
    vert.scaledLocalPos = skinnedPosition.xyz * scale;
    vert.localNormal = skinnedNormal;

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
    const device DirLight* directionalLights [[buffer(0)]],
    const device PointLight* pointLights [[buffer(1)]],
    const device Cluster* clusters [[buffer(2)]],
    constant CameraData& camera [[buffer(3)]],
    constant float2& screenSize [[buffer(4)]],
    constant packed_uint3& gridSize [[buffer(5)]],
    constant float& time [[buffer(6)]]
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

    float3 result = float3(0.0);
    float shadowFactor = texShadow.sample(s, screenUV).r;
    result += CalculateDirectionalLight(directionalLights[0], norm, T, B, viewDir, surf) * shadowFactor;

    uint tileX = uint(screenUV.x * float(gridSize.x));
    uint tileY = uint((1.0 - screenUV.y) * float(gridSize.y));
    uint tileIndex = tileX + tileY * gridSize.x;
    Cluster tile = clusters[tileIndex];
    uint lightCount = tile.lightCount;
    for (uint i = 0; i < lightCount; i++) {
        uint lightIndex = tile.lightIndices[i];
        result += CalculatePointLight(pointLights[lightIndex], norm, T, B, viewDir, surf, in.worldPosition.xyz);
    }

    result += float3(0.2) * surf.ao * surf.color;
    result += surf.emission;

    return float4(result, 1.0);
}
