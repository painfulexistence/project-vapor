#include <metal_stdlib>
using namespace metal;

// Water planar reflection — MSL twin of WaterRefl.vert / WaterRefl.frag.
// See those files for the design notes.
//
// Bindings through the RHI (mirrors the PSSM shadow pass conventions):
//   vertex:   buffer(0) = WaterData, buffer(1) = MaterialData[],
//             buffer(2) = InstanceData[], buffer(3) = VertexData[] (raw fetch),
//             buffer(4) = push { uint instanceID; uint useMergedOffset; }
//   fragment: buffer(0) = WaterData, buffer(1) = MaterialData[],
//             texture(0) = per-draw albedo, texture(1) = irradiance cube,
//             sampler(0..1)

struct ReflCameraPush {
    uint instanceID;
    uint useMergedOffset;  // MDI layout: add instances[iid].vertexOffset
};

struct ReflWave {
    float3 direction;
    float steepness;
    float waveLength;
    float amplitude;
    float speed;
};

struct ReflWaterData {
    float4x4 modelMatrix;
    float4 surfaceColor;
    float4 refractionColor;
    float4 ssrSettings;
    float4 normalMapScroll;
    float2 normalMapScrollSpeed;
    float2 _pad1;
    float refractionDistortionFactor;
    float refractionHeightFactor;
    float refractionDistanceFactor;
    float depthSofteningDistance;
    float foamHeightStart;
    float foamFadeDistance;
    float foamTiling;
    float foamAngleExponent;
    float roughness;
    float reflectance;
    float specIntensity;
    float foamBrightness;
    ReflWave waves[4];
    uint waveCount;
    float dampeningFactor;
    float time;
    float _pad2;
    float4 sunDirection;       // xyz = world dir FROM the sun
    float4 sunColorIntensity;  // rgb + w = intensity
    float4 causticsParams;     // w = water level Y
    float4 causticsBoundsMin;
    float4 causticsBoundsMax;
    float4x4 reflectionViewProj;
    float4 fftParams;
    float4 reflectionParams;
};

// Vapor::VertexData — 48 tightly packed bytes.
struct ReflVertexData {
    packed_float3 position;
    packed_float2 uv;
    packed_float3 normal;
    packed_float4 tangent;
};

// Vapor::InstanceData prefix (model + color + the uint block we need).
struct ReflInstanceData {
    float4x4 model;
    float4 color;
    uint vertexOffset;
    uint indexOffset;
    uint vertexCount;
    uint indexCount;
    uint materialID;
    uint primitiveMode;
    uint2 _pad1;
    packed_float3 aabbMin;
    float _pad2;
    packed_float3 aabbMax;
    float _pad3;
    float4 boundingSphere;
};

// Vapor::MaterialData — 112 bytes.
struct ReflMaterialData {
    float4 baseColorFactor;
    float normalScale;
    float metallicFactor;
    float roughnessFactor;
    float occlusionStrength;
    packed_float3 emissiveFactor;
    float alphaCutoff;
    float emissiveStrength;
    float subsurface;
    float specular;
    float specularTint;
    float anisotropic;
    float sheen;
    float sheenTint;
    float clearcoat;
    float clearcoatGloss;
    float prototypeUVMode;
    float uvScale;
    float iblEnabled;
    float transmission;
};

struct ReflVSOut {
    float4 position [[position]];
    float3 worldPos;
    float3 normal;
    float2 uv;
    uint materialID [[flat]];
};

vertex ReflVSOut vertexMain(
    uint vertexID [[vertex_id]],
    constant ReflWaterData& water [[buffer(0)]],
    device const ReflInstanceData* instances [[buffer(2)]],
    device const ReflVertexData* verts [[buffer(3)]],
    constant ReflCameraPush& push [[buffer(4)]]
) {
    ReflVSOut out;
    ReflInstanceData inst = instances[push.instanceID];
    uint vid = vertexID + (push.useMergedOffset != 0u ? inst.vertexOffset : 0u);

    float4 world = inst.model * float4(float3(verts[vid].position), 1.0);
    float3x3 nm = float3x3(inst.model[0].xyz, inst.model[1].xyz, inst.model[2].xyz);

    out.position = water.reflectionViewProj * world;
    out.worldPos = world.xyz;
    out.normal = normalize(nm * float3(verts[vid].normal));
    out.uv = float2(verts[vid].uv);
    out.materialID = inst.materialID;
    return out;
}

constant float REFL_PI = 3.14159265359;

fragment float4 fragmentMain(
    ReflVSOut in [[stage_in]],
    constant ReflWaterData& water [[buffer(0)]],
    device const ReflMaterialData* materials [[buffer(1)]],
    texture2d<float, access::sample> albedoMap [[texture(0)]],
    texturecube<float, access::sample> irradianceMap [[texture(1)]]
) {
    constexpr sampler linearWrapSampler(address::repeat, filter::linear, mip_filter::linear);
    constexpr sampler linearClampSampler(address::clamp_to_edge, filter::linear, mip_filter::linear);

    // Waterline clip: nothing below the surface belongs in the mirror.
    if (in.worldPos.y < water.causticsParams.w - 0.08) {
        discard_fragment();
    }

    ReflMaterialData mat = materials[in.materialID];
    float4 albedoSample = albedoMap.sample(linearWrapSampler, in.uv);
    if (albedoSample.a < mat.alphaCutoff) {
        discard_fragment();
    }
    float3 albedo = pow(albedoSample.rgb, float3(2.2)) * mat.baseColorFactor.rgb;

    float3 N = normalize(in.normal);
    float3 ambient = irradianceMap.sample(linearClampSampler, N).rgb;
    float3 sunDir = -normalize(water.sunDirection.xyz);
    float3 sun = water.sunColorIntensity.rgb * water.sunColorIntensity.w
               * max(dot(N, sunDir), 0.0) / REFL_PI;
    float3 emissive = float3(mat.emissiveFactor) * mat.emissiveStrength;

    return float4(albedo * (ambient + sun) + emissive, 1.0);
}
