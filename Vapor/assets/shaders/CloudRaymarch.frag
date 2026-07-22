#version 450
// Volumetric clouds — quarter-res raymarch. GLSL port of the Metal
// cloudFragmentLowRes path in 3d_volumetric_clouds.metal (Horizon-style:
// procedural Perlin-Worley shapes, dual-lobe phase, multi-scatter approx,
// Beer-powder). Outputs vec4(inscattering, transmittance) into cloudRT; a
// temporal pass then blends with history and a composite pass applies it to
// the scene. All tunables mirror the Metal-tested VolumetricCloudData.

layout(location = 0) in vec2 tex_uv;
layout(location = 0) out vec4 outCloud;

layout(set = 2, binding = 0) uniform sampler2D sceneDepth;

// Must match Vapor::VolumetricCloudRenderData (std430).
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
    float sunLightScale;  // cloud-specific scale on sunIntensity (< 1: clouds occlude)
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
    vec3 ambientColor;  // cloud ambient tint (weather-driven), scaled by ambientIntensity
    float _p9;
};

// Must match Vapor::CameraRenderData.
struct CameraData {
    mat4 proj; mat4 view; mat4 invProj; mat4 invView;
    float nearPlane; float farPlane; vec2 _pad;
    vec3 position; float _pad2;
    vec4 frustumPlanes[6];
};
layout(std430, set = 1, binding = 3) readonly buffer CameraBuf { CameraData cam; };

const float VOL_PI = 3.14159265359;
const float VOL_INV_4PI = 1.0 / (4.0 * VOL_PI);

float saturate(float x) { return clamp(x, 0.0, 1.0); }

// ── Noise (ports of 3d_volumetric_common.metal) ────────────────────────────
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

float gradientNoise3D(vec3 p) {
    vec3 pi = floor(p);
    vec3 pf = fract(p);
    vec3 w = pf * pf * pf * (pf * (pf * 6.0 - 15.0) + 10.0);
    float n000 = dot(hash33(pi + vec3(0,0,0)) * 2.0 - 1.0, pf - vec3(0,0,0));
    float n100 = dot(hash33(pi + vec3(1,0,0)) * 2.0 - 1.0, pf - vec3(1,0,0));
    float n010 = dot(hash33(pi + vec3(0,1,0)) * 2.0 - 1.0, pf - vec3(0,1,0));
    float n110 = dot(hash33(pi + vec3(1,1,0)) * 2.0 - 1.0, pf - vec3(1,1,0));
    float n001 = dot(hash33(pi + vec3(0,0,1)) * 2.0 - 1.0, pf - vec3(0,0,1));
    float n101 = dot(hash33(pi + vec3(1,0,1)) * 2.0 - 1.0, pf - vec3(1,0,1));
    float n011 = dot(hash33(pi + vec3(0,1,1)) * 2.0 - 1.0, pf - vec3(0,1,1));
    float n111 = dot(hash33(pi + vec3(1,1,1)) * 2.0 - 1.0, pf - vec3(1,1,1));
    return mix(mix(mix(n000, n100, w.x), mix(n010, n110, w.x), w.y),
               mix(mix(n001, n101, w.x), mix(n011, n111, w.x), w.y), w.z);
}

float worleyNoise3D(vec3 p) {
    vec3 pi = floor(p);
    vec3 pf = fract(p);
    float minDist = 1.0;
    for (int z = -1; z <= 1; z++)
    for (int y = -1; y <= 1; y++)
    for (int x = -1; x <= 1; x++) {
        vec3 offset = vec3(x, y, z);
        vec3 diff = offset + hash33(pi + offset) - pf;
        minDist = min(minDist, dot(diff, diff));
    }
    return sqrt(minDist);
}

float remap(float value, float inMin, float inMax, float outMin, float outMax) {
    return outMin + (value - inMin) * (outMax - outMin) / (inMax - inMin);
}

float interleavedGradientNoise(vec2 screenPos, uint frame) {
    vec3 magic = vec3(0.06711056, 0.00583715, 52.9829189);
    return fract(magic.z * fract(dot(screenPos + float(frame) * 5.588238, magic.xy)));
}

// ── Phase / extinction ─────────────────────────────────────────────────────
float phaseHG(float cosTheta, float g) {
    float g2 = g * g;
    return VOL_INV_4PI * (1.0 - g2) / pow(1.0 + g2 - 2.0 * g * cosTheta, 1.5);
}

float phaseDualLobe(float cosTheta, float g1, float g2, float blend) {
    return mix(phaseHG(cosTheta, g1), phaseHG(cosTheta, g2), blend);
}

float beerLambert(float density, float stepSize) { return exp(-density * stepSize); }

float beerPowderEnergy(float density, float cosTheta) {
    float powder = 1.0 - exp(-density * 2.0);
    return mix(1.0, powder, saturate(-cosTheta * 0.5 + 0.5));
}

// ── Cloud density (faithful ports) ─────────────────────────────────────────
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
    vec3 shapeUV = samplePos * shapeNoiseScale * 0.0001;
    float perlin = gradientNoise3D(shapeUV * 4.0) * 0.5 + 0.5;
    float worley1 = 1.0 - worleyNoise3D(shapeUV * 4.0);
    float worley2 = 1.0 - worleyNoise3D(shapeUV * 8.0);
    float worley3 = 1.0 - worleyNoise3D(shapeUV * 16.0);
    float worleyFBM = worley1 * 0.625 + worley2 * 0.25 + worley3 * 0.125;
    return saturate(remap(perlin, worleyFBM - 1.0, 1.0, 0.0, 1.0));
}

// Curl-ish vector noise: three decorrelated gradient noises. Not a true
// divergence-free curl, but visually equivalent wind-torn wisps at a third
// of the cost of a finite-difference curl.
vec3 curlDistort(vec3 p) {
    return vec3(gradientNoise3D(p),
                gradientNoise3D(p + vec3(31.416, 47.853, 12.793)),
                gradientNoise3D(p + vec3(-23.144, 9.271, 61.043)));
}

float sampleCloudDetail(vec3 worldPos) {
    vec3 samplePos = worldPos + windOffset * 1.5;
    // Wind-torn edges: distort the detail lookup with large-scale vector noise
    // (~500 m swirls at curlNoiseScale 1; strength 0.1 → ~30 m displacement).
    // Full-quality samples only — the cheap light-march path skips detail.
    if (curlNoiseStrength > 0.0) {
        samplePos += curlDistort(samplePos * (curlNoiseScale * 0.002)) *
                     (curlNoiseStrength * 300.0);
    }
    vec3 detailUV = samplePos * detailNoiseScale * 0.001;
    float d1 = 1.0 - worleyNoise3D(detailUV * 2.0);
    float d2 = 1.0 - worleyNoise3D(detailUV * 4.0);
    float d3 = 1.0 - worleyNoise3D(detailUV * 8.0);
    return d1 * 0.625 + d2 * 0.25 + d3 * 0.125;
}

vec2 sampleWeather(vec3 worldPos) {
    vec2 weatherUV = worldPos.xz * 0.00005 + time * 0.001;
    float coverage = valueNoise3D(vec3(weatherUV * 3.0, 0.0));
    coverage = pow(coverage * 0.5 + 0.5, 0.5);
    float type = valueNoise3D(vec3(weatherUV * 2.0 + 100.0, 0.0));
    type = type * 0.5 + 0.5;
    return vec2(coverage * cloudCoverage, type);
}

float sampleCloudDensity(vec3 worldPos, bool useCheap) {
    float heightFraction = saturate((worldPos.y - cloudLayerBottom) / cloudLayerThickness);
    if (heightFraction <= 0.0 || heightFraction >= 1.0) return 0.0;

    vec2 weather = sampleWeather(worldPos);
    float coverage = weather.x;
    float type = mix(weather.y, cloudType, 0.5);

    float heightGradient = cloudHeightGradient(heightFraction, type);
    float baseShape = sampleCloudShape(worldPos);
    float baseCloud = saturate(remap(baseShape * heightGradient, 1.0 - coverage, 1.0, 0.0, 1.0));

    if (useCheap || baseCloud <= 0.0) return baseCloud * cloudDensity;

    float detail = sampleCloudDetail(worldPos);
    float erosion = erosionStrength * (1.0 - heightFraction) * 0.5;
    float finalDensity = remap(baseCloud, detail * erosion, 1.0, 0.0, 1.0);
    return saturate(finalDensity) * cloudDensity;
}

// ── Lighting ───────────────────────────────────────────────────────────────
float lightMarch(vec3 worldPos) {
    float stepSize = cloudLayerThickness / float(lightSteps);
    float transmittance = 1.0;
    vec3 pos = worldPos;
    for (uint i = 0u; i < lightSteps; i++) {
        pos += sunDirection * stepSize;
        if (pos.y > cloudLayerTop) break;
        float density = sampleCloudDensity(pos, true);
        transmittance *= beerLambert(density, stepSize);
        if (transmittance < 0.01) break;
    }
    return transmittance;
}

vec3 multiScatterApprox(float lightTransmittance, float cosTheta) {
    float phase = phaseDualLobe(cosTheta, phaseG1, phaseG2, phaseBlend);
    // sunLightScale < 1: clouds absorb/self-shadow, so their lit surface sits
    // BELOW the clear-sky brightness instead of blooming over it.
    float sunPower = sunIntensity * sunLightScale;
    vec3 directLight = sunColor * sunPower * phase * lightTransmittance;

    vec3 multiScatter = vec3(0.0);
    float attenuation = 0.3;
    float contribution = 0.4;
    float phaseAttenuation = 0.5;
    float scatterPhase = phase;
    float scatterTransmittance = lightTransmittance;
    for (int i = 0; i < 4; i++) {
        scatterPhase = mix(scatterPhase, 0.25, phaseAttenuation);
        scatterTransmittance = mix(scatterTransmittance, 1.0, 0.7);
        multiScatter += contribution * scatterPhase * scatterTransmittance * sunColor;
        contribution *= attenuation;
    }

    float silverLining = pow(saturate(1.0 - lightTransmittance), silverLiningSpread);
    silverLining *= saturate(-cosTheta * 0.5 + 0.5);
    multiScatter += sunColor * silverLiningIntensity * silverLining;

    vec3 ambient = ambientColor * ambientIntensity;
    return directLight + multiScatter * sunPower + ambient;
}

// ── Raymarch ───────────────────────────────────────────────────────────────
vec2 cloudLayerIntersection(vec3 rayOrigin, vec3 rayDir) {
    float tBottom = (cloudLayerBottom - rayOrigin.y) / rayDir.y;
    float tTop = (cloudLayerTop - rayOrigin.y) / rayDir.y;
    float tMin = min(tBottom, tTop);
    float tMax = max(tBottom, tTop);
    if (rayOrigin.y >= cloudLayerBottom && rayOrigin.y <= cloudLayerTop) tMin = 0.0;
    return vec2(max(0.0, tMin), max(0.0, tMax));
}

vec4 raymarchClouds(vec3 rayOrigin, vec3 rayDir, float maxDist, float blueNoise) {
    vec2 tRange = cloudLayerIntersection(rayOrigin, rayDir);
    if (tRange.y <= tRange.x || tRange.x > maxDist) return vec4(0.0, 0.0, 0.0, 1.0);
    tRange.y = min(tRange.y, maxDist);

    float rayLength = tRange.y - tRange.x;
    float stepSize = rayLength / float(primarySteps);
    float t = tRange.x + stepSize * blueNoise;

    vec3 scattering = vec3(0.0);
    float transmittance = 1.0;
    float cosTheta = dot(rayDir, sunDirection);

    for (uint i = 0u; i < primarySteps && t < tRange.y; i++) {
        vec3 pos = rayOrigin + rayDir * t;
        float density = sampleCloudDensity(pos, false);
        if (density > 0.001) {
            float lightTransmittance = lightMarch(pos);
            vec3 luminance = multiScatterApprox(lightTransmittance, cosTheta);
            float powder = beerPowderEnergy(density * stepSize * 10.0, cosTheta) * powderStrength +
                           (1.0 - powderStrength);
            float stepTransmittance = beerLambert(density, stepSize);
            scattering += transmittance * luminance * (1.0 - stepTransmittance) * powder;
            transmittance *= stepTransmittance;
            if (transmittance < 0.01) { transmittance = 0.0; break; }
        }
        t += stepSize;
    }
    return vec4(scattering, transmittance);
}

void main() {
    float depth = texture(sceneDepth, tex_uv).r;

    // World-space view ray (same convention as VolumetricFog/Velocity).
    vec2 ndc = vec2(tex_uv.x * 2.0 - 1.0, 1.0 - tex_uv.y * 2.0);
    vec4 worldDir4 = invViewProj * vec4(ndc, 1.0, 1.0);
    vec3 rayDir = normalize(worldDir4.xyz / worldDir4.w - cameraPosition);

    float linearDepth = cam.nearPlane * cam.farPlane /
                        (cam.farPlane - depth * (cam.farPlane - cam.nearPlane));
    float maxDist = (depth >= 0.9999) ? 100000.0 : linearDepth;

    float blueNoise = interleavedGradientNoise(gl_FragCoord.xy, frameIndex);

    outCloud = raymarchClouds(cameraPosition, rayDir, maxDist, blueNoise);
}
