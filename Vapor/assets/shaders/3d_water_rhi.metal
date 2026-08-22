#include <metal_stdlib>
using namespace metal;

// RHI water surface — MSL twin of Water.vert / Water.frag. See those files
// for the design notes; the algorithm and the WaterData layout are identical
// (the CPU struct in graphics_effects.hpp is the single source of truth).
//
// Bindings through the RHI:
//   vertex:   buffer(0) = CameraRenderData, buffer(1) = WaterData,
//             buffer(2) = WaterVertexData[] (pulled by vertex_id),
//             buffer(3) = FFT displacement float4[] (manual bilinear)
//   fragment: buffer(0) = WaterData, buffer(1) = CameraRenderData,
//             texture(0..8) = simNormalFoam, detail1, detail2, sceneColor,
//             sceneDepth, envCube, reflectionMap, foam, noise

struct CameraDataRHI {
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

struct WaveRHI {
    float3 direction;
    float steepness;
    float waveLength;
    float amplitude;
    float speed;
};

struct WaterDataRHI {
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
    WaveRHI waves[4];
    uint waveCount;
    float dampeningFactor;
    float time;
    float _pad2;
    float4 sunDirection;
    float4 sunColorIntensity;
    float4 causticsParams;
    float4 causticsBoundsMin;
    float4 causticsBoundsMax;
    float4x4 reflectionViewProj;
    float4 fftParams;         // x=patch size y=detail tiling z=detail strength w=displacement scale
    float4 reflectionParams;  // x=planar strength y=distortion z=whitecap scale w=FFT resolution
};

struct WaterVertexRHI {
    packed_float3 position;
    packed_float2 uv0;
    packed_float2 uv1;
};

struct WaterVSOut {
    float4 position [[position]];
    float4 positionView;
    float4 texCoord0;        // xy: patch UV, zw: grid UV
    float4 screenPosition;
    float4 positionWorld;    // w: edge dampening
    float4 dispSample;
};

static float3 waterSampleDisplacement(float2 patchUV, uint n, device const float4* dispBuf) {
    if (n == 0u) return float3(0.0);
    float2 f = fract(patchUV) * float(n) - 0.5;
    float2 fl = floor(f);
    float2 fr = f - fl;
    int2 i0 = int2(fl);
    uint x0 = uint((i0.x % int(n) + int(n)) % int(n));
    uint y0 = uint((i0.y % int(n) + int(n)) % int(n));
    uint x1 = (x0 + 1u) % n;
    uint y1 = (y0 + 1u) % n;
    float3 d00 = dispBuf[y0 * n + x0].xyz;
    float3 d10 = dispBuf[y0 * n + x1].xyz;
    float3 d01 = dispBuf[y1 * n + x0].xyz;
    float3 d11 = dispBuf[y1 * n + x1].xyz;
    return mix(mix(d00, d10, fr.x), mix(d01, d11, fr.x), fr.y);
}

vertex WaterVSOut vertexMain(
    uint vertexID [[vertex_id]],
    constant CameraDataRHI& camera [[buffer(0)]],
    constant WaterDataRHI& water [[buffer(1)]],
    device const WaterVertexRHI* vertices [[buffer(2)]],
    device const float4* dispBuf [[buffer(3)]]
) {
    WaterVSOut out;

    float4 position = float4(float3(vertices[vertexID].position), 1.0);
    float4 texCoord0 = float4(float2(vertices[vertexID].uv0), float2(vertices[vertexID].uv1));

    float dampening = 1.0 - pow(saturate(abs(texCoord0.z - 0.5) / 0.5), water.dampeningFactor);
    dampening *= 1.0 - pow(saturate(abs(texCoord0.w - 0.5) / 0.5), water.dampeningFactor);

    float4 baseWorld = water.modelMatrix * position;
    float2 patchUV = baseWorld.xz / max(water.fftParams.x, 1e-3);

    uint n = uint(water.reflectionParams.w + 0.5);
    float3 d = waterSampleDisplacement(patchUV, n, dispBuf) * (dampening * water.fftParams.w);

    out.positionWorld = float4(baseWorld.xyz + d, dampening);
    out.positionView = camera.view * float4(out.positionWorld.xyz, 1.0);
    out.position = camera.proj * out.positionView;
    out.screenPosition = out.position;
    out.texCoord0 = float4(patchUV, texCoord0.zw);
    out.dispSample = float4(d, 0.0);

    return out;
}

// ── Fragment helpers ────────────────────────────────────────────────────────

constant float WATER_PI = 3.14159265359;
constant float WATER_EPSILON = 0.0001;

static float waterNDGGX(float linearRoughness, float nDotH) {
    float a2 = linearRoughness * linearRoughness;
    float d = (nDotH * nDotH) * (a2 - 1.0) + 1.0;
    return a2 / (WATER_PI * d * d);
}

static float waterSmithGGX(float linearRoughness, float nDotL, float nDotV) {
    float k = linearRoughness * 0.5;
    return (nDotL / (nDotL * (1.0 - k) + k)) * (nDotV / (nDotV * (1.0 - k) + k));
}

static float2 waterClipToScreenUV(float4 clipPos) {
    float2 ndc = clipPos.xy / clipPos.w;
    return float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
}

static float3 waterWorldPosFromDepth(float2 uv, float depth, constant CameraDataRHI& camera) {
    float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    float4 viewPos = camera.invProj * float4(ndc, depth, 1.0);
    viewPos /= viewPos.w;
    return (camera.invView * float4(viewPos.xyz, 1.0)).xyz;
}

fragment float4 fragmentMain(
    WaterVSOut in [[stage_in]],
    constant WaterDataRHI& water [[buffer(0)]],
    constant CameraDataRHI& camera [[buffer(1)]],
    texture2d<float, access::sample> simNormalFoam [[texture(0)]],
    texture2d<float, access::sample> detailNormal1 [[texture(1)]],
    texture2d<float, access::sample> detailNormal2 [[texture(2)]],
    texture2d<float, access::sample> sceneColor [[texture(3)]],
    texture2d<float, access::sample> sceneDepth [[texture(4)]],
    texturecube<float, access::sample> envMap [[texture(5)]],
    texture2d<float, access::sample> reflectionMap [[texture(6)]],
    texture2d<float, access::sample> foamMap [[texture(7)]],
    texture2d<float, access::sample> noiseMap [[texture(8)]]
) {
    constexpr sampler linearWrapSampler(address::repeat, filter::linear, mip_filter::linear);
    constexpr sampler pointClampSampler(address::clamp_to_edge, filter::nearest);
    constexpr sampler linearClampSampler(address::clamp_to_edge, filter::linear, mip_filter::linear);

    float dampening = saturate(in.positionWorld.w);
    float2 patchUV = in.texCoord0.xy;

    // ── Surface normal: FFT sim + scrolling detail layers ───────────────────
    float4 simSample = simNormalFoam.sample(linearWrapSampler, patchUV);
    float3 N = normalize(simSample.xyz);
    N = normalize(mix(float3(0.0, 1.0, 0.0), N, dampening));

    float2 det1UV = patchUV * water.fftParams.y + water.time * water.normalMapScroll.xy * water.normalMapScrollSpeed.x;
    float2 det2UV = patchUV * water.fftParams.y + water.time * water.normalMapScroll.zw * water.normalMapScrollSpeed.y;
    float3 dn1 = detailNormal1.sample(linearWrapSampler, det1UV).rgb * 2.0 - 1.0;
    float3 dn2 = detailNormal2.sample(linearWrapSampler, det2UV).rgb * 2.0 - 1.0;
    float2 detXZ = (dn1.xy + dn2.xy) * (0.5 * water.fftParams.z * dampening);
    N = normalize(float3(N.x + detXZ.x, N.y, N.z + detXZ.y));

    float3 viewDirWorld = normalize(camera.position - in.positionWorld.xyz);
    float2 hdrCoords = waterClipToScreenUV(in.screenPosition);

    // ── Fresnel ─────────────────────────────────────────────────────────────
    float f0 = 0.16 * water.reflectance * water.reflectance;
    float nDotVWorld = saturate(dot(N, viewDirWorld));
    float fresnel = f0 + (1.0 - f0) * pow(1.0 - nDotVWorld, 5.0);

    // ── Reflection: planar RT with normal distortion, env cube fallback ─────
    float3 envDir = reflect(-viewDirWorld, N);
    float3 envColor = envMap.sample(linearClampSampler, envDir, level(water.roughness * 4.0)).rgb
                    * water.causticsBoundsMax.w;

    float3 reflectionColor = envColor;
    if (water.reflectionParams.x > 0.0) {
        float4 rclip = water.reflectionViewProj * float4(in.positionWorld.xyz, 1.0);
        if (rclip.w > WATER_EPSILON) {
            float2 ruv = waterClipToScreenUV(rclip) + N.xz * water.reflectionParams.y;
            float2 border = min(ruv, 1.0 - ruv);
            float inRT = saturate(min(border.x, border.y) * 12.0);
            float3 planar = reflectionMap.sample(linearClampSampler,
                                                 clamp(ruv, float2(0.001), float2(0.999))).rgb;
            reflectionColor = mix(envColor, planar, inRT * water.reflectionParams.x);
        }
    }
    reflectionColor *= water.surfaceColor.rgb;

    // ── Refraction: scene snapshot with depth-based absorption ──────────────
    float2 distortedUV = hdrCoords + N.xz * water.refractionDistortionFactor;
    float distortedDepth = sceneDepth.sample(pointClampSampler, distortedUV).r;
    float3 distortedPos = waterWorldPosFromDepth(distortedUV, distortedDepth, camera);
    float2 refractionUV = (distortedPos.y < in.positionWorld.y) ? distortedUV : hdrCoords;
    float3 refractionColor = sceneColor.sample(linearClampSampler, refractionUV).rgb * water.refractionColor.rgb;

    float sceneDepthHere = sceneDepth.sample(pointClampSampler, hdrCoords).r;
    float3 scenePos = waterWorldPosFromDepth(hdrCoords, sceneDepthHere, camera);
    float depthSoftenedAlpha = saturate(length(scenePos - in.positionWorld.xyz) / water.depthSofteningDistance);

    float3 refractedFloorPos = (distortedPos.y < in.positionWorld.y) ? distortedPos : scenePos;
    refractionColor = mix(refractionColor, water.refractionColor.rgb,
                          saturate((in.positionWorld.y - refractedFloorPos.y) / water.refractionHeightFactor));

    // ── Sun specular (GGX) ──────────────────────────────────────────────────
    float3 lightDirWorld = -normalize(water.sunDirection.xyz);
    float3 halfVec = normalize(viewDirWorld + lightDirWorld);
    float nDotL = saturate(dot(N, lightDirWorld));
    float nDotV = abs(dot(N, viewDirWorld)) + WATER_EPSILON;
    float nDotH = saturate(dot(N, halfVec));
    float linearRoughness = max(water.roughness * water.roughness, 1e-4);
    float specularNoise = noiseMap.sample(linearWrapSampler, det1UV * 0.5).r
                        * noiseMap.sample(linearWrapSampler, det2UV * 0.5).r;
    float3 specular = float3(waterNDGGX(linearRoughness, nDotH) * waterSmithGGX(linearRoughness, nDotL, nDotV))
                    * fresnel * water.specIntensity * nDotL * (0.5 + specularNoise)
                    * water.sunColorIntensity.rgb * water.sunColorIntensity.w;

    // ── Combine ─────────────────────────────────────────────────────────────
    float3 color = mix(refractionColor, reflectionColor, saturate(fresnel)) + specular;

    // ── Foam: whitecaps (displacement Jacobian) + shoreline ─────────────────
    float whitecap = simSample.a * water.reflectionParams.z;
    float shore = pow(1.0 - depthSoftenedAlpha, 3.0);
    float foamAmount = saturate(whitecap + shore)
                     * noiseMap.sample(linearWrapSampler, patchUV * water.foamTiling).r;
    float3 foamColor = foamMap.sample(linearWrapSampler, patchUV * water.foamTiling + N.xz * 0.1).rgb
                     * water.foamBrightness;
    color = mix(color, foamColor, saturate(foamAmount) * depthSoftenedAlpha);

    return float4(color, depthSoftenedAlpha);
}
