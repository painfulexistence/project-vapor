#include <metal_stdlib>
using namespace metal;

// Water caustics — MSL twin of WaterCaustics.frag. See that file for the
// design notes. Fullscreen triangle over the HDR colorRT -> tempColorRT
// (the pass swaps them afterwards, HeightFog pattern).
//
// Bindings through the RHI: fragment buffer(0) = WaterData, buffer(1) =
// CameraRenderData, texture(0..2) = sceneColor, sceneDepth, sceneNormal.

struct CausticsCameraData {
    float4x4 proj;
    float4x4 view;
    float4x4 invProj;
    float4x4 invView;
    float nearPlane;
    float farPlane;
    float2 _pad;
    float3 position;
    float _pad2;
};

struct CausticsWave {
    float3 direction;
    float steepness;
    float waveLength;
    float amplitude;
    float speed;
};

struct CausticsWaterData {
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
    CausticsWave waves[4];
    uint waveCount;
    float dampeningFactor;
    float time;
    float _pad2;
    float4 sunDirection;       // xyz = world dir FROM the sun
    float4 sunColorIntensity;  // rgb + w = intensity
    float4 causticsParams;     // x=intensity y=world scale z=speed w=water level Y
    float4 causticsBoundsMin;  // xyz = water AABB min, w = fade depth
    float4 causticsBoundsMax;  // xyz = water AABB max, w = env reflection intensity
};

struct CausticsVSOut {
    float4 position [[position]];
    float2 uv;
};

constant float2 causticsVerts[3] = { float2(-1.0, -1.0), float2(3.0, -1.0), float2(-1.0, 3.0) };

vertex CausticsVSOut vertexMain(uint vertexID [[vertex_id]]) {
    CausticsVSOut out;
    out.position = float4(causticsVerts[vertexID], 0.0, 1.0);
    out.uv = causticsVerts[vertexID] * 0.5 + 0.5;
    out.uv.y = 1.0 - out.uv.y;  // y-down uv, same convention as the GLSL twin
    return out;
}

// Iterated-lattice caustic intensity (same as the GLSL twin).
static float causticPattern(float2 p, float t) {
    float2 i = p;
    float c = 1.0;
    const float inten = 0.005;
    for (int n = 0; n < 4; n++) {
        float phase = t * (1.0 - (3.5 / float(n + 1)));
        i = p + float2(cos(phase - i.x) + sin(phase + i.y),
                       sin(phase - i.y) + cos(phase + i.x));
        float2 denom = float2(sin(i.x + phase) / inten, cos(i.y + phase) / inten);
        c += 1.0 / max(length(float2(p.x / denom.x, p.y / denom.y)), 1e-5);
    }
    c /= 4.0;
    c = 1.17 - pow(c, 1.4);
    // MUST stay clamped: c is unbounded (the 1/length term spikes wherever the
    // lattice denominator crosses zero), so pow(|c|, 8) reaches ~1e12 at pool
    // coordinates and the multiplicative boost below turns every submerged
    // pixel pure white. The pattern is a 0..1 mask, nothing more.
    return clamp(pow(abs(c), 8.0), 0.0, 1.0);
}

fragment float4 fragmentMain(
    CausticsVSOut in [[stage_in]],
    constant CausticsWaterData& water [[buffer(0)]],
    constant CausticsCameraData& camera [[buffer(1)]],
    texture2d<float, access::sample> sceneColor [[texture(0)]],
    texture2d<float, access::sample> sceneDepth [[texture(1)]],
    texture2d<float, access::sample> sceneNormal [[texture(2)]]
) {
    constexpr sampler linearClampSampler(address::clamp_to_edge, filter::linear);
    constexpr sampler pointClampSampler(address::clamp_to_edge, filter::nearest);

    float4 scene = sceneColor.sample(linearClampSampler, in.uv);
    float depth = sceneDepth.sample(pointClampSampler, in.uv).r;
    if (depth >= 0.9999 || water.causticsParams.x <= 0.0) {
        return scene;
    }

    float2 ndc = float2(in.uv.x * 2.0 - 1.0, 1.0 - in.uv.y * 2.0);
    float4 viewPos = camera.invProj * float4(ndc, depth, 1.0);
    viewPos /= viewPos.w;
    float3 worldPos = (camera.invView * float4(viewPos.xyz, 1.0)).xyz;

    float waterLevel = water.causticsParams.w;
    float3 bmin = water.causticsBoundsMin.xyz;
    float3 bmax = water.causticsBoundsMax.xyz;

    const float edge = 0.15;
    float mask = smoothstep(0.0, edge, worldPos.x - bmin.x)
               * smoothstep(0.0, edge, bmax.x - worldPos.x)
               * smoothstep(0.0, edge, worldPos.z - bmin.z)
               * smoothstep(0.0, edge, bmax.z - worldPos.z)
               * smoothstep(0.0, 0.05, waterLevel - worldPos.y)
               * smoothstep(0.0, edge, worldPos.y - bmin.y + edge);
    if (mask <= 0.001) {
        return scene;
    }

    float3 n = normalize(sceneNormal.sample(pointClampSampler, in.uv).xyz);
    float nDotL = saturate(dot(n, -normalize(water.sunDirection.xyz)));
    float facing = 0.25 + 0.75 * nDotL;

    float submersion = waterLevel - worldPos.y;
    float fadeDepth = max(water.causticsBoundsMin.w, 0.01);
    float depthFade = pow(saturate(1.0 - submersion / fadeDepth), 1.5);

    float2 p = worldPos.xz * water.causticsParams.y;
    float t = water.time * water.causticsParams.z;

    float cG = causticPattern(p, t);
    float cR = causticPattern(p + float2(0.008, 0.004), t);
    float cB = causticPattern(p - float2(0.006, 0.008), t);
    float3 caustic = float3(cR, cG, cB);

    float3 sunTint = normalize(water.sunColorIntensity.rgb + float3(0.001)) * 1.732;
    float3 boost = caustic * sunTint * (water.causticsParams.x * mask * facing * depthFade);

    return float4(scene.rgb * (float3(1.0) + boost), scene.a);
}
