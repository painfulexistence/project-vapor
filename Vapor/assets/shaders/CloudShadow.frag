#version 450
// Cloud shadow map: sun-light transmittance through the cloud deck, rendered
// top-down over a camera-centered world region (CSM_HALF half-extent, snapped
// to the texel grid). Each texel marches from its ground point toward the sun
// through the cloud layer with the CHEAP density (no detail/curl erosion) and
// writes exp(-tau) — the PBR passes multiply the sun term by it, so drifting
// cloud shadows sweep across the ground. Twin of cloudShadowMap in
// 3d_volumetric_clouds.metal; density/noise functions are copies of the
// CloudRaymarch.frag cheap path and must stay in sync with it.

layout(location = 0) in vec2 tex_uv;
layout(location = 0) out float outTransmittance;

// Baked tileable Perlin-Worley base shape (same volume the raymarch samples).
layout(set = 2, binding = 0) uniform sampler3D shapeNoiseTex;
// Weather map twin of CloudRaymarch.frag's (R = coverage base, G = type).
layout(set = 2, binding = 1) uniform sampler2D weatherMapTex;

// Must match Vapor::VolumetricCloudRenderData (std430) — CloudRaymarch twin.
layout(std430, set = 1, binding = 0) readonly buffer CloudBuf {
    mat4 invViewProj;
    mat4 prevViewProj;
    vec3 cameraPosition;  float _p1;
    vec3 sunDirection;    float _p2;
    vec3 sunColor;        float _p3;
    float sunIntensity;
    float cloudLayerBottom;
    float cloudLayerTop;
    float cloudLayerThickness;
    float cloudCoverage;
    float cloudDensity;
    float cloudType;
    float erosionStrength;
    float shapeNoiseScale;
    float detailNoiseScale;
    float curlNoiseScale;
    float curlNoiseStrength;
    float ambientIntensity;
    float silverLiningIntensity;
    float silverLiningSpread;
    float phaseG1;
    float phaseG2;
    float phaseBlend;
    float powderStrength;
    float sunLightScale;
    vec3 windDirection;   float _p5;
    vec3 windOffset;      float _p6;
    float windSpeed;
    float time;
    uint primarySteps;
    uint lightSteps;
    vec2 screenSize;
    vec2 _p7;
    uint frameIndex;
    float temporalBlend;
    vec2 _p8;
    vec3 ambientColor;
    float _p9;
    vec3 moonColor;
    float moonLightScale;
};

// Must match Vapor::CameraRenderData.
struct CameraData {
    mat4 proj; mat4 view; mat4 invProj; mat4 invView;
    float nearPlane; float farPlane; vec2 _pad;
    vec3 position; float _pad2;
    vec4 frustumPlanes[6];
};
layout(std430, set = 1, binding = 3) readonly buffer CameraBuf { CameraData cam; };

// World half-extent covered by the map and the snap grid — MUST match the
// sampling constants in RHIMain.frag / the Metal PBR fragments.
const float CSM_HALF = 2048.0;
const float CSM_SNAP = 16.0;

float saturate(float x) { return clamp(x, 0.0, 1.0); }

// ── Noise (cheap-density subset; keep in sync with CloudRaymarch.frag) ──────
float hash13(vec3 p3) {
    p3 = fract(p3 * 0.1031);
    p3 += dot(p3, p3.zyx + 31.32);
    return fract((p3.x + p3.y) * p3.z);
}

vec3 hash33(vec3 p3) {
    p3 = fract(p3 * vec3(0.1031, 0.1030, 0.0973));
    p3 += dot(p3, p3.yxz + 33.33);
    return fract((p3.xxy + p3.yxx) * p3.zyx);
}

float valueNoise3D(vec3 p) {
    vec3 pi = floor(p);
    vec3 pf = fract(p);
    vec3 w = pf * pf * (3.0 - 2.0 * pf);
    return mix(
        mix(mix(hash13(pi + vec3(0,0,0)), hash13(pi + vec3(1,0,0)), w.x),
            mix(hash13(pi + vec3(0,1,0)), hash13(pi + vec3(1,1,0)), w.x), w.y),
        mix(mix(hash13(pi + vec3(0,0,1)), hash13(pi + vec3(1,0,1)), w.x),
            mix(hash13(pi + vec3(0,1,1)), hash13(pi + vec3(1,1,1)), w.x), w.y),
        w.z);
}

float remap(float value, float inMin, float inMax, float outMin, float outMax) {
    return outMin + (value - inMin) * (outMax - outMin) / (inMax - inMin);
}

// ── Cheap cloud density (CloudRaymarch.frag twins, detail/curl-free path) ───
float cloudHeightGradient(float heightFraction, float type) {
    float stratus = remap(heightFraction, 0.0, 0.1, 0.0, 1.0) * remap(heightFraction, 0.2, 0.3, 1.0, 0.0);
    float stratocumulus = remap(heightFraction, 0.0, 0.1, 0.0, 1.0) * remap(heightFraction, 0.4, 0.6, 1.0, 0.0);
    float cumulus = remap(heightFraction, 0.0, 0.1, 0.0, 1.0) * remap(heightFraction, 0.7, 0.95, 1.0, 0.0);
    float gradient = mix(stratus, cumulus, type);
    gradient = mix(gradient, stratocumulus, saturate(type * 2.0) * (1.0 - saturate(type * 2.0 - 1.0)));
    return saturate(gradient);
}

float sampleCloudShape(vec3 worldPos) {
    vec3 samplePos = worldPos + windOffset;
    return texture(shapeNoiseTex, samplePos * (shapeNoiseScale * 0.0001)).r;
}

vec2 sampleWeather(vec3 worldPos) {
    vec2 weatherUV = (worldPos.xz + windOffset.xz * 0.6) * 0.00005 + time * 0.0002;
    vec2 w = texture(weatherMapTex, weatherUV).rg;
    return vec2(w.r * cloudCoverage, w.g);
}

float sampleCloudDensityCheap(vec3 worldPos) {
    float heightFraction = saturate((worldPos.y - cloudLayerBottom) / cloudLayerThickness);
    if (heightFraction <= 0.0 || heightFraction >= 1.0) return 0.0;
    vec2 weather = sampleWeather(worldPos);
    float type = mix(weather.y, cloudType, 0.5);
    float heightGradient = cloudHeightGradient(heightFraction, type);
    float baseShape = sampleCloudShape(worldPos);
    float baseCloud = saturate(remap(baseShape * heightGradient, 1.0 - weather.x, 1.0, 0.0, 1.0));
    return baseCloud * cloudDensity;
}

void main() {
    // Sun below/near the horizon: no directional cloud shadow (night is lit by
    // the dim moon — not worth a second march), fade rather than pop.
    float dayFactor = smoothstep(-0.12, 0.08, sunDirection.y);
    if (dayFactor < 0.01 || sunDirection.y < 0.05) {
        outTransmittance = 1.0;
        return;
    }

    vec2 center = floor(cam.position.xz / CSM_SNAP) * CSM_SNAP;
    vec3 world = vec3(center.x + (tex_uv.x * 2.0 - 1.0) * CSM_HALF,
                      0.0,
                      center.y + (tex_uv.y * 2.0 - 1.0) * CSM_HALF);

    // March the ground point toward the sun through the cloud layer.
    float t0 = (cloudLayerBottom - world.y) / sunDirection.y;
    float t1 = (cloudLayerTop    - world.y) / sunDirection.y;
    const int STEPS = 6;
    float stepLen = (t1 - t0) / float(STEPS);
    float tau = 0.0;
    for (int i = 0; i < STEPS; i++) {
        vec3 pos = world + sunDirection * (t0 + (float(i) + 0.5) * stepLen);
        tau += sampleCloudDensityCheap(pos) * stepLen;
    }
    outTransmittance = mix(1.0, exp(-tau), dayFactor);
}
