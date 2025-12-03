#pragma once
#include <SDL3/SDL_stdinc.h>
#include <array>
#include <cstddef>
#include <glm/geometric.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>
#include <vector>

enum class AlphaMode {
    OPAQUE,
    MASK,
    BLEND,
};

enum class PrimitiveMode {
    POINTS,
    LINES,
    LINE_STRIP,
    TRIANGLES,
    TRIANGLE_STRIP,
};

struct PipelineHandle {
    Uint32 rid = UINT32_MAX;
};

struct BufferHandle {
    Uint32 rid = UINT32_MAX;
};

struct TextureHandle {
    Uint32 rid = UINT32_MAX;
};

struct RenderTargetHandle {
    Uint32 rid = UINT32_MAX;
};

struct Image {
    std::string uri;
    Uint32 width;
    Uint32 height;
    Uint32 channelCount;
    std::vector<Uint8> byteArray;
    TextureHandle texture;
};

struct Material {
    std::string name;
    AlphaMode alphaMode;
    float alphaCutoff;
    bool doubleSided;
    glm::vec4 baseColorFactor = glm::vec4(1.0f);
    float normalScale = 1.0f;
    float metallicFactor = 1.0f;
    float roughnessFactor = 1.0f;
    float occlusionStrength = 1.0f;
    glm::vec3 emissiveFactor = glm::vec3(0.0f);
    float emissiveStrength = 1.0f;
    std::shared_ptr<Image> albedoMap;
    std::shared_ptr<Image> normalMap;
    std::shared_ptr<Image> metallicMap;
    std::shared_ptr<Image> roughnessMap;
    std::shared_ptr<Image> occlusionMap;
    std::shared_ptr<Image> emissiveMap;
    std::shared_ptr<Image> displacementMap;
    float subsurface = 0.0f;
    float specular = 0.5f;
    float specularTint = 0.0f;
    float anisotropic = 0.0f;
    float sheen = 0.0f;
    float sheenTint = 0.5f;
    float clearcoat = 0.0f;
    float clearcoatGloss;
    bool usePrototypeUV = false;
    // std::string shaderPath;
    PipelineHandle pipeline;
};

struct alignas(16) MaterialData {
    glm::vec4 baseColorFactor;
    float normalScale;
    float metallicFactor;
    float roughnessFactor;
    float occlusionStrength;
    glm::vec3 emissiveFactor;
    float _pad1;
    float emissiveStrength;
    float subsurface;
    float specular;
    float specularTint;
    float anisotropic;
    float sheen;
    float sheenTint;
    float clearcoat;
    float clearcoatGloss;
    float usePrototypeUV;
};

struct alignas(16) DirectionalLight {// Note that alignas(16) is not enough to ensure 16-byte alignment
    glm::vec3 direction;
    float _pad1;
    glm::vec3 color;
    float _pad2;
    float intensity;
    // float _pad3[3];
    // bool castShadow;
    // Uint8 _pad4[3];
};

struct alignas(16) PointLight {// Note that alignas(16) is not enough to ensure 16-byte alignment
    glm::vec3 position;
    float _pad1;
    glm::vec3 color;
    float _pad2;
    float intensity = 1.0f;
    float radius = 0.5f;
    // float _pad3[2];
    // bool castShadow;
    // Uint8 _pad4[3];
};

struct alignas(16) FrameData {
    Uint32 frameNumber;
    float time;
    float deltaTime;
};

struct alignas(16) CameraData {
    glm::mat4 proj;
    glm::mat4 view;
    glm::mat4 invProj;
    glm::mat4 invView;
    float near;
    float far;
    glm::vec3 position;
    float _pad1;
    glm::vec4 frustumPlanes[6];
};

struct alignas(16) InstanceData {
    glm::mat4 model;
    glm::vec4 color;
    Uint32 vertexOffset;
    Uint32 indexOffset;
    Uint32 vertexCount;
    Uint32 indexCount;
    Uint32 materialID;
    PrimitiveMode primitiveMode;
    Uint32 _pad1[2];
    glm::vec3 AABBMin;
    float _pad2;
    glm::vec3 AABBMax;
    float _pad3;
    glm::vec4 boundingSphere;// x, y, z, radius
};

struct alignas(16) Cluster {
    glm::vec4 min;
    glm::vec4 max;
    Uint32 lightCount;
    Uint32 lightIndices[256];
};

struct alignas(16) LightCullData {
    glm::vec2 screenSize;
    glm::vec2 _pad1;
    glm::uvec3 gridSize;
    Uint32 lightCount;
};

// Water rendering data structures
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
    glm::vec4 surfaceColor;// Water surface tint color
    glm::vec4 refractionColor;// Deep water color
    glm::vec4 ssrSettings;// x: step size, y: max steps, z: refinement steps, w: distance factor
    glm::vec4 normalMapScroll;// xy: scroll direction 1, zw: scroll direction 2
    glm::vec2 normalMapScrollSpeed;// Scroll speeds for both normal maps
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
    // float tessellationFactor;
    WaveData waves[4];// Up to 4 waves
    Uint32 waveCount;
    float dampeningFactor;
    float time;
};

// Water transform (CPU-side, used to build modelMatrix)
struct WaterTransform {
    glm::vec3 position = glm::vec3(0.0f, 0.0f, 0.0f);
    glm::vec3 scale = glm::vec3(1.0f, 1.0f, 1.0f);
};

// Atmosphere rendering data for Rayleigh and Mie scattering
struct alignas(16) AtmosphereData {
    glm::vec3 sunDirection;// Normalized sun direction
    float _pad1;
    glm::vec3 sunColor;// Sun light color
    float _pad2;
    float sunIntensity;// Sun light intensity
    float planetRadius;// Planet radius in meters (Earth: 6371e3)
    float atmosphereRadius;// Atmosphere radius in meters (Earth: 6471e3)
    float exposure;// Exposure for tone mapping
    glm::vec3 rayleighCoefficients;// Rayleigh scattering coefficients (Earth: 5.8e-6, 13.5e-6, 33.1e-6)
    float _pad3;
    float rayleighScaleHeight;// Rayleigh scale height (Earth: 8500)
    float mieCoefficient;// Mie scattering coefficient (Earth: 21e-6)
    float mieScaleHeight;// Mie scale height (Earth: 1200)
    float miePreferredDirection;// Mie preferred scattering direction (g parameter, typically 0.758)
    glm::vec3 groundColor;// Ground color for horizon and IBL (default: greenish)
    float _pad4;
};

// IBL capture data for rendering atmosphere to cubemap
struct alignas(16) IBLCaptureData {
    glm::mat4 viewProj;// View-projection matrix for current cubemap face
    Uint32 faceIndex;// Current cubemap face (0-5)
    float roughness;// Roughness level for prefilter pass
    float _pad[2];
};

struct VertexData {
    glm::vec3 position;
    glm::vec2 uv;
    glm::vec3 normal;
    glm::vec4 tangent;
    // glm::vec3 bitangent;
};

// Water vertex data with two UV channels
// uv0: per-tile tiling coordinates
// uv1: whole grid coordinates (0-1 across entire grid)
struct WaterVertexData {
    glm::vec3 position;
    glm::vec2 uv0;// Tiled UV for normal map scrolling
    glm::vec2 uv1;// Grid UV for edge dampening
};

// GPU Particle for compute shader simulation
struct alignas(16) GPUParticle {
    glm::vec3 position = glm::vec3(0.0f);
    float _pad1 = 0.0f;
    glm::vec3 velocity = glm::vec3(0.0f);
    float _pad2 = 0.0f;
    glm::vec3 force = glm::vec3(0.0f);
    float _pad3 = 0.0f;
    glm::vec4 color = glm::vec4(1.0f);
};

// Particle simulation parameters (uniform buffer)
struct alignas(16) ParticleSimulationParams {
    glm::vec2 resolution = glm::vec2(1280.0f, 720.0f);
    glm::vec2 mousePosition = glm::vec2(0.0f);
    float time = 0.0f;
    float deltaTime = 0.0f;
    float _pad1 = 0.0f;
    float _pad2 = 0.0f;
};

// Attractor data for particle simulation
struct alignas(16) ParticleAttractorData {
    glm::vec3 position = glm::vec3(0.0f);
    float strength = 1.0f;
};

// Particle push constants for rendering
struct ParticlePushConstants {
    float particleSize = 0.05f;
    float _pad1 = 0.0f;
    float _pad2 = 0.0f;
    float _pad3 = 0.0f;
};

// Legacy CPU particle (for compatibility)
struct Particle {
    glm::vec3 position = glm::vec3(1.0f);
    glm::vec3 velocity = glm::vec3(1.0f);
    glm::vec3 density = glm::vec3(1.0f);
};

// struct alignas(16) DrawCommand {
//     Uint32 indexCount;
//     Uint32 instanceCount;
//     Uint32 indexStart;
//     Uint32 baseVertex;
//     Uint32 baseInstance;
// };

struct Mesh {
    void initialize(const std::vector<VertexData>& vertices, const std::vector<Uint32>& indices);
    void initialize(VertexData* vertexData, size_t vertexCount, Uint32* indexData, size_t indexCount);
    void calculateNormals();
    void calculateTangents();
    void calculateLocalAABB();
    glm::vec4 getWorldBoundingSphere() const;
    void print();

    bool hasPosition = false;
    bool hasNormal = false;
    bool hasTangent = false;
    bool hasUV0 = false;
    bool hasUV1 = false;
    bool hasColor = false;
    std::vector<VertexData> vertices;// interleaved vertex data
    std::vector<Uint32> indices;
    std::shared_ptr<Material> material = nullptr;
    PrimitiveMode primitiveMode;
    glm::vec3 localAABBMin;
    glm::vec3 localAABBMax;
    glm::vec3 worldAABBMin;
    glm::vec3 worldAABBMax;
    bool isGeometryDirty = true;

    // GPU-driven rendering
    Uint32 vertexOffset = 0;
    Uint32 indexOffset = 0;
    Uint32 vertexCount = 0;
    Uint32 indexCount = 0;

    // Runtime data
    std::vector<BufferHandle> vbos;
    BufferHandle ebo;
    Uint32 instanceID = UINT32_MAX;// also used as blas index
    Uint32 materialID = UINT32_MAX;
};