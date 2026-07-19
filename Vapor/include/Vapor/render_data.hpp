#pragma once
#include <SDL3/SDL_stdinc.h>
#include <glm/glm.hpp>
#include <vector>
#include "rhi.hpp"
#include "graphics.hpp"

// ============================================================================
// Renderer Layer Data Structures
//
// This layer sits between the Application (Scene) and RHI.
// It manages rendering resources and implements high-level rendering logic.
// ============================================================================

// ============================================================================
// Renderer Resource IDs
// ============================================================================

namespace Vapor {

using MeshId = Uint32;
using MaterialId = Uint32;
using TextureId = Uint32;

constexpr Uint32 INVALID_MESH_ID = UINT32_MAX;
constexpr Uint32 INVALID_MATERIAL_ID = UINT32_MAX;
constexpr Uint32 INVALID_TEXTURE_ID = UINT32_MAX;

// ============================================================================
// Drawable - Represents a single renderable object
// ============================================================================

struct Drawable {
    glm::mat4 transform;
    MeshId mesh = INVALID_MESH_ID;
    MaterialId material = INVALID_MATERIAL_ID;
    glm::vec3 aabbMin;
    glm::vec3 aabbMax;
    glm::vec4 color = glm::vec4(1.0f);
    Uint32 vertexOffset = 0;
    Uint32 indexOffset = 0;
};

// ============================================================================
// RenderMesh - Renderer's internal mesh representation
// ============================================================================

struct RenderMesh {
    BufferHandle vertexBuffer;
    BufferHandle indexBuffer;
    Uint32 indexCount = 0;
    Uint32 vertexCount = 0;
    Uint32 vertexOffset = 0;
    Uint32 indexOffset = 0;
    // Range into the global meshlet buffers (meshlet path). Shared by every
    // instance of this mesh; the task shader looks it up by mesh id. 0/0 when the
    // mesh has no baked meshlet data.
    Uint32 meshletOffset = 0;
    Uint32 meshletCount = 0;
};

// ============================================================================
// RenderTexture - Renderer's internal texture representation
// ============================================================================

struct RenderTexture {
    TextureHandle handle;
    SamplerHandle sampler;
    Uint32 width = 0;
    Uint32 height = 0;
    PixelFormat format = PixelFormat::RGBA8_UNORM;
};

// ============================================================================
// Material Flags
// ============================================================================

enum MaterialFlags : Uint32 {
    HAS_ALBEDO_TEXTURE = 1 << 0,
    HAS_NORMAL_MAP = 1 << 1,
    HAS_METALLIC_MAP = 1 << 2,
    HAS_ROUGHNESS_MAP = 1 << 3,
    HAS_OCCLUSION_MAP = 1 << 4,
    HAS_EMISSIVE_MAP = 1 << 5,
    ALPHA_BLEND = 1 << 6,
    DOUBLE_SIDED = 1 << 7,
};

// ============================================================================
// RenderMaterial - Renderer's internal material representation
// ============================================================================

struct RenderMaterial {
    // Material parameters (CPU-side cache)
    glm::vec4 baseColorFactor = glm::vec4(1.0f);
    float normalScale = 1.0f;
    float metallicFactor = 1.0f;
    float roughnessFactor = 1.0f;
    float occlusionStrength = 1.0f;
    glm::vec3 emissiveFactor = glm::vec3(0.0f);
    float emissiveStrength = 1.0f;
    float subsurface = 0.0f;
    float specular = 0.5f;
    float specularTint = 0.0f;
    float anisotropic = 0.0f;
    float sheen = 0.0f;
    float sheenTint = 0.5f;
    float clearcoat = 0.0f;
    float clearcoatGloss = 1.0f;
    float transmission = 0.0f;  // KHR_materials_transmission (rendering only)
    bool useIBL = false;  // native Material::useIBL default (graphics.hpp)

    // Texture references (IDs, not handles)
    TextureId albedoTexture = INVALID_TEXTURE_ID;
    TextureId normalTexture = INVALID_TEXTURE_ID;
    TextureId metallicTexture = INVALID_TEXTURE_ID;
    TextureId roughnessTexture = INVALID_TEXTURE_ID;
    TextureId occlusionTexture = INVALID_TEXTURE_ID;
    TextureId emissiveTexture = INVALID_TEXTURE_ID;

    // GPU resources
    BufferHandle parameterBuffer;
    DescriptorSetHandle descriptorSet;
    PipelineHandle pipeline;

    // Metadata
    MaterialFlags flags = static_cast<MaterialFlags>(0);
    Vapor::AlphaMode alphaMode = Vapor::AlphaMode::OPAQUE;
    float alphaCutoff = 0.5f;
    bool doubleSided = false;
};

// ============================================================================
// Camera Data for Renderer
// ============================================================================

struct CameraRenderData {
    glm::mat4 proj;
    glm::mat4 view;
    glm::mat4 invProj;
    glm::mat4 invView;
    float nearPlane;
    float farPlane;
    float _pad[2];
    glm::vec3 position;
    float _pad2;
    glm::vec4 frustumPlanes[6];
};

// ============================================================================
// Light Data for Renderer
// ============================================================================

struct DirectionalLightData {
    glm::vec3 direction;
    float _pad1;
    glm::vec3 color;
    float _pad2;
    float intensity;
    float _pad3[3];
};

struct PointLightData {
    glm::vec3 position;
    float _pad1;
    glm::vec3 color;
    float _pad2;
    float intensity;
    float radius;
    float _pad3[2];
};

// PSSM cascade data consumed by the PBR fragment shader (matches the Metal
// shader's PSSMData). The neutral default (cascadeSplits = +inf) keeps every
// pixel in the "RT shadow" branch, which samples the (white) shadow texture —
// i.e. fully lit until the shadow passes are ported.
struct alignas(16) PSSMRenderData {
    glm::mat4 lightSpaceMatrices[3] = { glm::mat4(1.0f), glm::mat4(1.0f), glm::mat4(1.0f) };
    glm::vec4 cascadeSplits = glm::vec4(3.0e38f);
    // Offsets 208-224 mirror the Metal PSSMData (3d_common.metal) EXACTLY so the
    // RHI-Metal PBR shader reads every field correctly. Previously nearShadowEnd
    // sat at 212 where the Metal shader reads cascadeBlendRange, aliasing the
    // cascade-blend width to the near distance (~25) and heavily over-blending.
    float  blendRange = 1.0f;         // 208  RT<->PSSM (unused on the RHI path)
    float  cascadeBlendRange = 2.0f;  // 212  cascade<->cascade blend (view units)
    Uint32 pcfSampleCount = 16;       // 216  PCF taps 4/8/16/32
    Uint32 debugVisualize = 0;        // 220  cascade-colour debug (0 = off)
    // Near-field shadow map data, appended after the Metal-shared prefix.
    float  nearShadowEnd = 0.0f;      // 224  covers [near, nearShadowEnd]; 0 = off
    float  _pad[3] = {};              // 228  pad so nearLightMatrix is 16-aligned
    glm::mat4 nearLightMatrix = glm::mat4(1.0f);  // 240
};

// Physically-based atmosphere (Rayleigh/Mie/Ozone) consumed by Atmosphere.frag.
// Layout matches the Metal backend's AtmosphereData (graphics_effects.hpp), with
// Earth-like defaults.
struct alignas(16) AtmosphereRenderData {
    glm::vec3 sunDirection = glm::normalize(glm::vec3(0.5f, 0.5f, 0.5f));
    float _pad1 = 0.0f;
    glm::vec3 sunColor = glm::vec3(1.0f);
    float _pad2 = 0.0f;
    float sunIntensity = 12.0f;
    float planetRadius = 6371e3f;
    float atmosphereRadius = 6471e3f;
    float exposure = 1.0f;
    glm::vec3 rayleighCoefficients = glm::vec3(5.8e-6f, 13.5e-6f, 33.1e-6f);
    float _pad3 = 0.0f;
    float rayleighScaleHeight = 8500.0f;
    float mieCoefficient = 21e-6f;
    float mieScaleHeight = 1200.0f;
    float miePreferredDirection = 0.758f;
    glm::vec3 groundColor = glm::vec3(0.015f, 0.015f, 0.02f);
    float _pad4 = 0.0f;
};

// Which sky the gameplay layer wants rendered and used for image-based lighting.
enum class SkyType : uint32_t {
    Atmosphere = 0,  // procedural Rayleigh/Mie/Ozone march (default)
    HDRI       = 1,  // equirectangular HDRI captured to the environment cubemap
    Gradient   = 2,  // cheap zenith/horizon/ground gradient
};

// Resolved sky description that SkySystem hands to the renderer whenever the
// authoring SkyComponent changes. Carries only what the sky itself owns — the
// sun (direction/color/intensity) stays light-driven (see LightGatherSystem),
// so these are the atmosphere tunables minus the sun fields, plus gradient
// colors. This is a CPU-side contract, not a GPU buffer (no alignment needed).
struct SkyRenderData {
    SkyType type = SkyType::Atmosphere;
    // Atmosphere tunables (mirror AtmosphereRenderData minus the sun fields).
    glm::vec3 rayleighCoefficients = glm::vec3(5.8e-6f, 13.5e-6f, 33.1e-6f);
    float rayleighScaleHeight = 8500.0f;
    float mieCoefficient = 21e-6f;
    float mieScaleHeight = 1200.0f;
    float miePreferredDirection = 0.758f;
    float planetRadius = 6371e3f;
    float atmosphereRadius = 6471e3f;
    float exposure = 1.0f;
    glm::vec3 groundColor = glm::vec3(0.015f, 0.015f, 0.02f);
    // Gradient sky colors (used when type == Gradient).
    glm::vec3 gradientZenith  = glm::vec3(0.18f, 0.34f, 0.62f);
    glm::vec3 gradientHorizon = glm::vec3(0.62f, 0.74f, 0.88f);
    glm::vec3 gradientGround  = glm::vec3(0.20f, 0.18f, 0.16f);
};

// GPU buffer for the gradient sky pass (SkyType::Gradient). Each color is
// vec4-padded so the std430 layout is identical on every backend (matches
// GradientData in Gradient.frag / 3d_gradient.metal).
struct alignas(16) GradientRenderData {
    glm::vec4 zenith  = glm::vec4(0.18f, 0.34f, 0.62f, 1.0f);
    glm::vec4 horizon = glm::vec4(0.62f, 0.74f, 0.88f, 1.0f);
    glm::vec4 ground  = glm::vec4(0.20f, 0.18f, 0.16f, 1.0f);
};

// Screen-space light scattering (god rays). Layout matches the Metal backend's
// LightScatteringData. sunScreenPos/screenSize are filled per frame.
struct alignas(16) LightScatteringRenderData {
    glm::vec2 sunScreenPos = glm::vec2(0.5f);
    glm::vec2 screenSize = glm::vec2(1.0f);
    float density = 1.0f;
    float weight = 0.01f;
    float decay = 0.97f;
    float exposure = 0.3f;
    uint32_t numSamples = 64;
    float maxDistance = 1.0f;
    float sunIntensity = 1.0f;
    float mieG = 0.76f;
    // Layout matches the MSL LightScatteringData: float3 occupies 16 bytes
    // there, so an explicit lane-4 pad keeps every following field at the
    // same offset on both backends (total size 96).
    glm::vec3 sunColor = glm::vec3(1.0f);
    float _sunColorPad = 0.0f;
    float _pad1 = 0.0f;
    float depthThreshold = 0.9999f;
    float jitter = 0.5f;
    float _pad3 = 0.0f;
    glm::vec2 _pad2 = glm::vec2(0.0f);
};

// AO temporal reprojection params (Vulkan AOTemporal.frag, set1 b0).
struct alignas(16) AOTemporalRenderData {
    glm::mat4 prevView = glm::mat4(1.0f);
    uint32_t historyValid = 0;
    uint32_t _pad[3] = {0, 0, 0};
};

// Simple screen-space height/distance fog (the Metal backend's simpleFog path).
// Shared wind, resolved from the ECS WindFieldComponent by WindSystem and
// pushed to the renderer. Wind DIRECTION is a single shared field — clouds, fog
// and particles all blow the same way. `strength` is the shared wind magnitude:
// each medium keeps its own per-medium scroll coefficient (the existing
// windSpeed on the fog/cloud settings) and multiplies it by this strength, so
// one knob drives everything while per-medium ratios are preserved.
struct WindRenderData {
    glm::vec3 direction  = glm::vec3(1.0f, 0.0f, 0.0f);
    float     strength   = 1.0f;
    float     turbulence = 0.0f;
};

struct alignas(16) FogRenderData {
    glm::mat4 invViewProj = glm::mat4(1.0f);
    glm::vec3 cameraPosition = glm::vec3(0.0f);
    float _p0 = 0.0f;
    glm::vec3 sunDirection = glm::normalize(glm::vec3(0.5f, 0.5f, 0.5f));
    float _p1 = 0.0f;
    glm::vec3 sunColor = glm::vec3(1.0f);
    float sunIntensity = 12.0f;
    float fogDensity = 0.02f;
    float fogHeightFalloff = 0.1f;
    float anisotropy = 0.6f;
    float ambientIntensity = 0.3f;
    // Height falloff shaping + wind-animated density noise (GLSL twin of the
    // Metal simpleFogFragment). windDirection comes from the shared
    // WindFieldComponent (setWind); windSpeed is the per-medium scroll
    // coefficient scaled by the shared wind strength at fill time; `time`
    // scrolls the noise. Defaults mirror the Metal fog so both backends match.
    float fogBaseHeight = 0.0f;    // 128
    float fogMaxHeight = 100.0f;   // 132
    float noiseScale = 0.01f;      // 136
    float noiseIntensity = 0.5f;   // 140
    float windSpeed = 1.0f;        // 144  per-medium scroll coefficient
    float time = 0.0f;             // 148
    glm::vec2 _pad2 = glm::vec2(0.0f);                        // 152 (align vec3 to 160)
    glm::vec3 windDirection = glm::vec3(1.0f, 0.0f, 0.0f);   // 160
    float _pad3 = 0.0f;                                       // 172 (struct = 176, /16)
};

// Heterogeneous volume raymarch (EmberGen-style density grid in an AABB).
// vec4-only layout so the MSL and std430 twins are byte-identical
// (VolumeRaymarch.frag / 3d_volume_raymarch.metal).
struct alignas(16) VolumeRenderData {
    glm::mat4 invViewProj = glm::mat4(1.0f);
    glm::vec4 cameraPosition = glm::vec4(0.0f);              // xyz
    glm::vec4 boxMin = glm::vec4(-2.0f, 0.0f, -2.0f, 8.0f);  // xyz; w = densityScale
    glm::vec4 boxMax = glm::vec4(2.0f, 4.0f, 2.0f, 0.3f);    // xyz; w = anisotropy
    glm::vec4 albedo = glm::vec4(0.9f, 0.9f, 0.9f, 0.15f);   // xyz; w = ambientIntensity
    glm::vec4 sunDirection = glm::vec4(0.0f, 1.0f, 0.0f, 0.0f);  // xyz
    glm::vec4 sunColor = glm::vec4(1.0f, 1.0f, 1.0f, 12.0f); // xyz; w = sunIntensity
    glm::vec4 params = glm::vec4(48.0f, 8.0f, 0.0f, 0.0f);   // x = steps, y = shadow steps
};

// Volumetric clouds. Field-for-field mirror of the Metal backend's
// VolumetricCloudData (graphics_effects.hpp) — the defaults below are the
// values already tuned/tested on the Metal renderer.
struct alignas(16) VolumetricCloudRenderData {
    glm::mat4 invViewProj = glm::mat4(1.0f);
    glm::mat4 prevViewProj = glm::mat4(1.0f);
    glm::vec3 cameraPosition = glm::vec3(0.0f);
    float _pad1 = 0.0f;
    glm::vec3 sunDirection = glm::normalize(glm::vec3(0.5f, 0.5f, 0.5f));
    float _pad2 = 0.0f;
    glm::vec3 sunColor = glm::vec3(1.0f);
    float _pad3 = 0.0f;
    float sunIntensity = 22.0f;
    float cloudLayerBottom = 1500.0f;
    float cloudLayerTop = 4000.0f;
    float cloudLayerThickness = 2500.0f;
    float cloudCoverage = 0.5f;
    float cloudDensity = 0.3f;
    float cloudType = 0.5f;
    float erosionStrength = 0.3f;
    float shapeNoiseScale = 1.0f;
    float detailNoiseScale = 5.0f;
    float curlNoiseScale = 1.0f;
    float curlNoiseStrength = 0.1f;
    float ambientIntensity = 0.3f;
    float silverLiningIntensity = 0.5f;
    float silverLiningSpread = 2.0f;
    float phaseG1 = 0.8f;
    float phaseG2 = -0.3f;
    float phaseBlend = 0.3f;
    float powderStrength = 0.5f;
    float _pad4 = 0.0f;
    glm::vec3 windDirection = glm::vec3(1.0f, 0.0f, 0.0f);
    float _pad5 = 0.0f;
    glm::vec3 windOffset = glm::vec3(0.0f);
    float _pad6 = 0.0f;
    float windSpeed = 10.0f;
    float time = 0.0f;
    Uint32 primarySteps = 64;
    Uint32 lightSteps = 6;
    glm::vec2 screenSize = glm::vec2(1.0f);
    glm::vec2 _pad7 = glm::vec2(0.0f);
    Uint32 frameIndex = 0;
    float temporalBlend = 0.05f;
    glm::vec2 _pad8 = glm::vec2(0.0f);
};

// IBL capture parameters (mirror of the Metal IBLCaptureData).
struct alignas(16) IBLCaptureRenderData {
    glm::mat4 viewProj = glm::mat4(1.0f);
    Uint32 faceIndex = 0;
    float roughness = 0.0f;
    float _pad[2] = {};
};

// Sun/lens flare parameters (clean-room redesign shared by SunFlare.frag and
// sunflare_rhi.metal — the old 30-knob procedural flare was scrapped). Sun
// occlusion is sampled from scene depth in-shader; sunScreenPos/aspect are
// filled per frame.
struct alignas(16) SunFlareRenderData {
    glm::vec2 sunScreenPos = glm::vec2(0.5f);   // uv, y-down
    glm::vec2 aspectRatio = glm::vec2(1.0f);    // (w/h, 1)
    // Explicit lane-4 pad after the vec3: the MSL FlareData float3 occupies
    // 16 bytes, so every following field sits 4 bytes later than std430
    // would place it. Total size 64 on both backends.
    glm::vec3 sunColor = glm::vec3(1.0f, 0.95f, 0.85f);
    float _sunColorPad = 0.0f;
    float intensity = 0.5f;        // overall flare radiance scale (pre-tonemap)
    float glowSize = 0.06f;        // gaussian core radius (aspect-corrected uv)
    float haloRadius = 0.22f;      // thin ring radius around the sun
    float ghostSpacing = 0.30f;    // ghost step along the sun->center axis
    float streakIntensity = 0.35f; // anamorphic horizontal streak strength
    float _pad0 = 0.0f;
    float _pad1 = 0.0f;
    float _pad2 = 0.0f;
};

// Maximum ECS attractors uploaded per frame.
static constexpr Uint32 MAX_PARTICLE_ATTRACTORS = 8;

// GPU particle (matches the Particle struct in ParticleForce/Integrate.comp and
// Particle.vert).  _pad1 repurposed as per-particle lifetime (-1 = immortal),
// _pad2 repurposed as current age; zero binary-layout change (64 bytes).
struct alignas(16) GPUParticleData {
    glm::vec3 position = glm::vec3(0.0f);
    float lifetime = -1.0f; // seconds before despawn; -1 = immortal
    glm::vec3 velocity = glm::vec3(0.0f);
    float age = 0.0f;       // seconds since spawn
    glm::vec3 force = glm::vec3(0.0f);
    float _pad3 = 0.0f;
    glm::vec4 color = glm::vec4(1.0f);
};

// Particle simulation params (bound as an SSBO in both backends).
// 64 bytes — std430/std140 safe.
struct alignas(16) ParticleSimParams {
    glm::vec2 resolution = glm::vec2(1280.0f, 720.0f); // offset 0
    glm::vec2 mousePosition = glm::vec2(0.0f);          // offset 8
    float time = 0.0f;                                   // offset 16
    float deltaTime = 1.0f / 60.0f;                     // offset 20
    Uint32 particleCount = 0;                            // offset 24
    Uint32 attractorCount = 0;                           // offset 28
    glm::vec4 wind = glm::vec4(0.0f);       // offset 32 — xyz=dir, w=strength
    glm::vec4 turbulence = glm::vec4(0.0f); // offset 48 — w=strength (xyz reserved)
};                                           // total: 64 bytes

// MSL's float3 occupies 16 bytes, so position must be padded to 16 and the
// struct rounds to 32 (strength at offset 16). The GLSL twin gets the same
// explicit pad; without this Metal asserted "attractor needs 32, got 16".
// Layout: 32 bytes — mirrors std430 Attractor struct in the shaders.
struct alignas(16) ParticleAttractor {
    glm::vec3 position = glm::vec3(0.0f);
    float _pad0 = 0.0f;      // offset 12 — keeps strength at offset 16
    float strength = 50.0f;  // offset 16
    float _pad1[3] = {0.0f, 0.0f, 0.0f}; // offset 20 — round to 32 bytes
};

// Per-frame particle force field — built by ParticleForceFieldSystem from ECS
// and handed to the renderer as a single packet.
struct ParticleForceField {
    std::vector<ParticleAttractor> attractors; // capped to MAX_PARTICLE_ATTRACTORS
    glm::vec4 wind        = glm::vec4(0.0f);  // xyz=direction, w=strength
    float     turbulence  = 0.0f;
};

// Maximum per-emitter particle draws submitted per frame (sizes the indirect
// args buffer; the draw list is truncated past this).
static constexpr Uint32 MAX_PARTICLE_DRAWS = 64;

// One per-emitter particle draw — built by ParticleRenderSystem from
// ParticleEmitterComponent + ParticleRendererComponent and handed to the
// renderer as the frame's draw list. Each packet becomes one draw with its
// own blend pipeline and texture (per-material draws); the slot range maps
// to instances via firstInstance = slotBegin.
struct ParticleDrawPacket {
    Uint32    slotBegin = 0;
    Uint32    slotCount = 0;
    Uint8     blendMode = 0;                    // ParticleBlendMode value
    TextureId texture   = INVALID_TEXTURE_ID;   // INVALID = procedural soft disc
    float     size      = 0.1f;                 // billboard half-extent
};

// ============================================================================
// Frustum for Culling
// ============================================================================

struct Frustum {
    glm::vec4 planes[6]; // left, right, bottom, top, near, far

    bool isBoxVisible(const glm::vec3& aabbMin, const glm::vec3& aabbMax) const {
        for (int i = 0; i < 6; ++i) {
            const glm::vec4& plane = planes[i];
            glm::vec3 positive = aabbMin;

            if (plane.x >= 0) positive.x = aabbMax.x;
            if (plane.y >= 0) positive.y = aabbMax.y;
            if (plane.z >= 0) positive.z = aabbMax.z;

            if (glm::dot(glm::vec3(plane), positive) + plane.w < 0) {
                return false;
            }
        }
        return true;
    }
};

// ============================================================================
// Material Data Input (from Application Layer)
// ============================================================================

struct MaterialDataInput {
    glm::vec4 baseColorFactor = glm::vec4(1.0f);
    float normalScale = 1.0f;
    float metallicFactor = 1.0f;
    float roughnessFactor = 1.0f;
    float occlusionStrength = 1.0f;
    glm::vec3 emissiveFactor = glm::vec3(0.0f);
    float emissiveStrength = 1.0f;
    float subsurface = 0.0f;
    float specular = 0.5f;
    float specularTint = 0.0f;
    float anisotropic = 0.0f;
    float sheen = 0.0f;
    float sheenTint = 0.5f;
    float clearcoat = 0.0f;
    float clearcoatGloss = 1.0f;
    float transmission = 0.0f;  // KHR_materials_transmission (rendering only)

    // Texture data (from Application's Image objects)
    std::shared_ptr<Vapor::Image> albedoMap;
    std::shared_ptr<Vapor::Image> normalMap;
    std::shared_ptr<Vapor::Image> metallicMap;
    std::shared_ptr<Vapor::Image> roughnessMap;
    std::shared_ptr<Vapor::Image> occlusionMap;
    std::shared_ptr<Vapor::Image> emissiveMap;

    Vapor::AlphaMode alphaMode = Vapor::AlphaMode::OPAQUE;
    float alphaCutoff = 0.5f;
    bool doubleSided = false;
};

} // namespace Vapor

// Transitional shim: these types lived at global scope before the namespace
// unification; unqualified call sites keep compiling while they migrate to
// Vapor:: qualification. Remove once call sites are migrated.
using namespace Vapor;
