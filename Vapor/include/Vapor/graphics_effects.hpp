#pragma once
// Effect-specific GPU data structures: water, atmosphere, volumetrics,
// particles, sun flare. Separated from core GPU structs to keep each file
// focused on a single visual system.
#include <SDL3/SDL_stdinc.h>
#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

// ── Water ─────────────────────────────────────────────────────────────────

namespace Vapor {

struct alignas(16) WaveData {
    glm::vec3 direction;
    float _pad1;
    float steepness;
    float waveLength;
    float amplitude;
    float speed;
};

struct alignas(16) WaterData {
    glm::mat4 modelMatrix;
    glm::vec4 surfaceColor;
    glm::vec4 refractionColor;
    glm::vec4 ssrSettings;
    glm::vec4 normalMapScroll;
    glm::vec2 normalMapScrollSpeed;
    glm::vec2 _pad1;
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
    WaveData waves[4];
    Uint32 waveCount;
    float dampeningFactor;
    float time;
};

struct WaterTransform {
    glm::vec3 position = glm::vec3(0.0f);
    glm::vec3 scale = glm::vec3(1.0f);
};

// ── Atmosphere ────────────────────────────────────────────────────────────

struct alignas(16) AtmosphereData {
    glm::vec3 sunDirection;
    float _pad1;
    glm::vec3 sunColor;
    float _pad2;
    float sunIntensity;
    float planetRadius;
    float atmosphereRadius;
    float exposure;
    glm::vec3 rayleighCoefficients;
    float _pad3;
    float rayleighScaleHeight;
    float mieCoefficient;
    float mieScaleHeight;
    float miePreferredDirection;
    glm::vec3 groundColor;
    float _pad4;
};

// ── Volumetric Fog ────────────────────────────────────────────────────────

// Froxel-fog globals: camera/frustum, sun, temporal, wind. The per-froxel fog
// tunables (fogDensity … ambientIntensity) are the "global height fog" volume's
// params and also feed the simpleFogFragment fallback; bounded volumes come from
// the separate VolumetricFogVolumeGPU array. Field-for-field twin of the MSL
// VolumetricFogData in 3d_volumetric_fog.metal (float3 occupies a full 16-byte
// slot in MSL, so each vec3 + trailing _pad below is one 16-byte member there).
struct alignas(16) VolumetricFogData {
    glm::mat4 invViewProj;
    glm::mat4 prevViewProj;
    glm::vec3 cameraPosition;
    float _pad1;
    glm::vec3 sunDirection;
    float _pad2;
    glm::vec3 sunColor;
    float _pad3;
    float sunIntensity;
    float fogDensity = 0.02f;
    float fogHeightFalloff = 0.1f;
    float fogBaseHeight = 0.0f;
    float fogMaxHeight = 100.0f;
    float scatteringCoeff = 0.5f;
    float extinctionCoeff = 0.5f;
    float anisotropy = 0.6f;
    float ambientIntensity = 0.3f;
    float nearPlane = 0.1f;
    float farPlane = 500.0f;
    Uint32 volumeCount = 0;   // fog volumes in the VolumetricFogVolumeGPU buffer (was _pad4)
    glm::vec2 screenSize;
    glm::vec2 _pad5;
    Uint32 frameIndex = 0;
    float temporalBlend = 0.1f;
    float noiseScale = 0.01f;
    float noiseIntensity = 0.5f;
    float windSpeed = 1.0f;
    float time = 0.0f;
    glm::vec2 _pad6;
    glm::vec3 windDirection = glm::vec3(1.0f, 0.0f, 0.0f);
    float _pad7;
};

// One fog volume injected into the froxel grid. vec4-only so the MSL and std430
// twins are byte-identical (96 bytes). boundsMin.w flags bounded vs global;
// bounded volumes carry a world-space AABB with soft edges. Twin of the MSL
// VolumetricFogVolume (3d_volumetric_fog.metal) and the std430 FogVolume in
// VolumetricFog.frag. Packed by the renderer from VolumetricFogVolumeData.
struct alignas(16) VolumetricFogVolumeGPU {
    glm::vec4 boundsMin;      // xyz = world AABB min; w = bounded flag (0 = global, 1 = AABB)
    glm::vec4 boundsMax;      // xyz = world AABB max; w = edgeFalloff (world units)
    glm::vec4 densityParams;  // x = density, y = heightFalloff, z = baseHeight, w = maxHeight
    glm::vec4 albedoBlend;    // xyz = single-scatter albedo/tint; w = blendWeight
    glm::vec4 phaseNoise;     // x = anisotropy, y = ambientIntensity, z = noiseScale, w = noiseIntensity
    glm::vec4 wind;           // x = windSpeed (already × global strength); yzw unused
};

// ── Volumetric Clouds ─────────────────────────────────────────────────────

struct alignas(16) VolumetricCloudData {
    glm::mat4 invViewProj;
    glm::mat4 prevViewProj;
    glm::vec3 cameraPosition;
    float _pad1;
    glm::vec3 sunDirection;
    float _pad2;
    glm::vec3 sunColor;
    float _pad3;
    float sunIntensity = 22.0f;
    // Hand-tuned base (see renderer_metal.cpp's volumetricCloudSettings): deep
    // 9500-15000 m layer, low coverage, near-zero ambient/silver-lining.
    float cloudLayerBottom = 9500.0f;
    float cloudLayerTop = 15000.0f;
    float cloudLayerThickness = 5500.0f;
    float cloudCoverage = 0.25f;
    float cloudDensity = 0.3f;
    float cloudType = 0.5f;
    float erosionStrength = 0.3f;
    float shapeNoiseScale = 1.0f;
    float detailNoiseScale = 5.0f;
    float curlNoiseScale = 1.0f;
    float curlNoiseStrength = 0.1f;
    float ambientIntensity = 0.001f;
    float silverLiningIntensity = 0.001f;
    float silverLiningSpread = 2.0f;
    float phaseG1 = 0.8f;
    float phaseG2 = -0.3f;
    float phaseBlend = 0.3f;
    float powderStrength = 0.5f;
    // Cloud-specific scale on sunIntensity (see VolumetricCloudRenderData).
    float sunLightScale = 0.85f;
    glm::vec3 windDirection = glm::vec3(1.0f, 0.0f, 0.0f);
    float _pad5;
    glm::vec3 windOffset;
    float _pad6;
    float windSpeed = 80.0f;
    float time = 0.0f;
    Uint32 primarySteps = 64;
    Uint32 lightSteps = 6;
    glm::vec2 screenSize;
    glm::vec2 _pad7;
    Uint32 frameIndex = 0;
    float temporalBlend = 0.05f;
    glm::vec2 _pad8;
    // Cloud ambient (sky-fill) tint, scaled by ambientIntensity.
    glm::vec3 ambientColor = glm::vec3(0.5f, 0.6f, 0.9f);
    float _pad9 = 0.0f;
    // Night key light (moonDir = -sunDirection); see VolumetricCloudRenderData.
    glm::vec3 moonColor = glm::vec3(0.92f, 0.93f, 1.0f);
    float moonLightScale = 0.04f;
};

// ── Light Scattering (God Rays) ───────────────────────────────────────────

struct alignas(16) LightScatteringData {
    glm::vec2 sunScreenPos;
    glm::vec2 screenSize;
    float density = 1.0f;
    float weight = 0.01f;
    float decay = 0.97f;
    float exposure = 0.3f;
    Uint32 numSamples = 64;
    float maxDistance = 1.0f;
    float sunIntensity = 1.0f;
    float mieG = 0.76f;
    glm::vec3 sunColor;
    float _pad1;
    float depthThreshold = 0.9999f;
    float jitter = 0.5f;
    glm::vec2 _pad2;
};

// ── Sun / Lens Flare ──────────────────────────────────────────────────────

struct alignas(16) SunFlareData {
    glm::vec2 sunScreenPos;
    glm::vec2 screenSize;
    glm::vec2 screenCenter = glm::vec2(0.5f);
    glm::vec2 aspectRatio = glm::vec2(1.0f);
    float sunIntensity = 1.0f;
    float visibility = 1.0f;
    float fadeEdge = 0.8f;
    float _pad1;
    glm::vec3 sunColor = glm::vec3(1.0f, 0.95f, 0.8f);
    float _pad2;
    float glowIntensity = 0.5f;
    float glowFalloff = 8.0f;
    float glowSize = 0.15f;
    float haloIntensity = 0.3f;
    float haloRadius = 0.25f;
    float haloWidth = 0.03f;
    float haloFalloff = 0.01f;
    Uint32 ghostCount = 6;
    float ghostSpacing = 0.3f;
    float ghostIntensity = 0.15f;
    float ghostSize = 0.05f;
    float ghostChromaticOffset = 0.005f;
    float ghostFalloff = 1.5f;
    float streakIntensity = 0.2f;
    float streakLength = 0.3f;
    float streakFalloff = 50.0f;
    float starburstIntensity = 0.15f;
    float starburstSize = 0.4f;
    Uint32 starburstPoints = 6;
    float starburstRotation = 0.0f;
    float dirtIntensity = 0.0f;
    float dirtScale = 10.0f;
    float time = 0.0f;
    float _pad3;
};

// ── Particles ─────────────────────────────────────────────────────────────

struct alignas(16) GPUParticle {
    glm::vec3 position = glm::vec3(0.0f);
    float _pad1 = 0.0f;
    glm::vec3 velocity = glm::vec3(0.0f);
    float _pad2 = 0.0f;
    glm::vec3 force = glm::vec3(0.0f);
    float _pad3 = 0.0f;
    glm::vec4 color = glm::vec4(1.0f);
};

// TODO: check alignment
struct alignas(16) ParticleSimulationParams {
    glm::vec2 resolution = glm::vec2(1280.0f, 720.0f);
    glm::vec2 mousePosition = glm::vec2(0.0f);
    float time = 0.0f;
    float deltaTime = 0.0f;
    Uint32 particleCount = 0;
};

struct alignas(16) ParticleAttractorData {
    glm::vec3 position = glm::vec3(0.0f);
    float _pad1;
    float strength = 1.0f;
};

struct ParticlePushConstants {
    float particleSize = 0.05f;
    float _pad1 = 0.0f;
    float _pad2 = 0.0f;
    float _pad3 = 0.0f;
};

// (The legacy CPU Particle struct that used to be duplicated here lives in
// graphics.hpp — one definition now that both headers share namespace Vapor.)

} // namespace Vapor

// Transitional shim: these types lived at global scope before the namespace
// unification; unqualified call sites keep compiling while they migrate to
// Vapor:: qualification. Remove once call sites are migrated.
using namespace Vapor;
