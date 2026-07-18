#pragma once
#include "rhi.hpp"  // TextureHandle (value member on Image; used by the native Metal renderer)
#include "meshlet.hpp"  // MeshletData (baked meshlet + cluster-LOD data on Mesh)
#include <SDL3/SDL_stdinc.h>
#include <glm/geometric.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>
#include <glm/mat4x4.hpp>
#include <cstddef>
#include <vector>
#include <array>
#include <string>
#include <memory>

namespace Vapor {

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

// High-level material shading model (selects the render pipeline downstream).
enum class MaterialType {
    PBR,
    Iridescent,
};

// Note: PipelineHandle, BufferHandle, TextureHandle are now defined in rhi.hpp
// RenderTargetHandle is no longer used (replaced by RHI's RenderPassDesc)

struct Image {
    std::string uri;
    Uint32 width;
    Uint32 height;
    Uint32 channelCount;
    std::vector<Uint8> byteArray;

    // GPU texture handle. The RHI renderer keeps its own path→TextureId cache
    // and ignores this; the native Metal renderer stores the uploaded texture
    // here (main's data model). Harmless/unused on the RHI path.
    TextureHandle texture;
};

// Floating-point image for HDR equirectangular environment maps (.hdr / .exr)
struct HDRImage {
    std::string uri;
    Uint32 width;
    Uint32 height;
    std::vector<float> floatArray; // 4 floats per pixel (RGBA, linear)
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
    float clearcoatGloss = 1.0f;

    // Prototype UV Mode: 0 = Off, 1 = World Space, 2 = Object Space
    int prototypeUVMode = 0;
    float uvScale = 1.0f;
    MaterialType materialType = MaterialType::PBR;
    bool useIBL = false;

    // Renderer-assigned ID (assigned during registration)
    Uint32 rendererMaterialId = UINT32_MAX;

    // Note: Pipeline and texture handles moved to Renderer layer
    // Material now only holds CPU-side material parameters
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
    // These three were on the shader's MaterialData (3d_common.metal) and the
    // global GPU struct, but had drifted off this Vapor:: copy. Because
    // alignas(16) already padded the struct to 96 bytes, the shader was
    // reading them out of that uninitialized tail: a garbage prototypeUVMode
    // (> 0.5) made the PBR shader replace every mesh UV with triplanar
    // projection — textures smeared, surfaces looked flat ("half the detail
    // gone"). Defaults keep un-set materials on their mesh UVs with no IBL.
    float prototypeUVMode = 0.0f;  // 0 = mesh UV, 1 = world-space, 2 = object-space
    float uvScale = 1.0f;
    float iblEnabled = 0.0f;       // 1 = image-based lighting, 0 = ambient approximation
};

struct alignas(16) DirectionalLight { // Note that alignas(16) is not enough to ensure 16-byte alignment
    glm::vec3 direction;
    float _pad1;
    glm::vec3 color;
    float _pad2;
    float intensity;
    // float _pad3[3];
    // bool castShadow;
    // Uint8 _pad4[3];
};

struct alignas(16) PointLight { // Note that alignas(16) is not enough to ensure 16-byte alignment
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

// Cone spot light. direction points FROM the light; cosInner/cosOuter are the
// cosines of the inner (full intensity) and outer (falloff-to-zero) half-angles.
// Layout is std430/MSL-clean (vec3 + trailing scalar packing), uploaded as-is.
// NOTE the explicit pads: MSL float3 occupies a 16-byte slot, so scalars must
// NOT be packed into a vec3's tail (the PointLight convention). RectLight
// keeps its tail-packed layout; its MSL twin uses packed_float3 to match.
struct alignas(16) SpotLight {
    glm::vec3 position{0.0f};
    float _pad0 = 0.0f;
    glm::vec3 direction{0.0f, -1.0f, 0.0f};   // normalized, FROM the light
    float _pad1 = 0.0f;
    glm::vec3 color{1.0f};
    float _pad2 = 0.0f;
    float radius = 10.0f;                     // range (world units)
    float cosInner = 0.9397f;                 // cos(20 deg)
    float cosOuter = 0.8660f;                 // cos(30 deg)
    float intensity = 1.0f;
};

// Rectangular area light driven by an optional video texture.
// right and up must be orthonormal; halfWidth/halfHeight are in world units.
struct alignas(16) RectLight {
    glm::vec3 position;
    float halfWidth;
    glm::vec3 right;           // normalized right axis
    float halfHeight;
    glm::vec3 up;              // normalized up axis
    float intensity;
    glm::vec3 color;
    Uint32 useVideoTexture;    // 0 = solid color, 1 = sample rectLightVideo texture
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
    glm::vec4 boundingSphere; // x, y, z, radius
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

struct VertexData {
    glm::vec3 position;
    glm::vec2 uv;
    glm::vec3 normal;
    glm::vec4 tangent;
    // glm::vec3 bitangent;
};

struct Particle {
    glm::vec3 position = glm::vec3(1.0f);
    glm::vec3 velocity = glm::vec3(1.0f);
    glm::vec3 density = glm::vec3(1.0f);
};

// GPU-driven rendering: indirect draw arguments produced by the cull compute
// pass. Packed 20-byte layout, matching VkDrawIndexedIndirectCommand and
// MTLDrawIndexedPrimitivesIndirectArguments exactly (intentionally NOT
// alignas(16) — the tight stride is what the indirect-draw APIs expect). Mirror
// of gpu::DrawCommand in graphics_gpu_structs.hpp.
struct DrawCommand {
    Uint32 indexCount;
    Uint32 instanceCount; // 0 = culled (GPU no-op)
    Uint32 firstIndex;
    Sint32 vertexOffset;
    Uint32 firstInstance; // = instance index (InstanceData lookup)
};
static_assert(sizeof(DrawCommand) == 20, "DrawCommand must match the GPU indirect-args layout");

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
    std::vector<VertexData> vertices; // interleaved vertex data
    std::vector<Uint32> indices;
    MeshletData meshletData;          // baked meshlet data model; unused/unserialized here
    std::shared_ptr<Material> material = nullptr;
    Uint32 renderMeshId = UINT32_MAX;
    Uint32 renderMaterialId = UINT32_MAX;
    PrimitiveMode primitiveMode;
    // Default to a large box so a mesh whose AABB was never computed is treated
    // as "always visible" rather than culled (a zero/uninitialized AABB collapses
    // to a point and gets frustum-culled the moment its origin leaves view).
    glm::vec3 localAABBMin = glm::vec3(-1e4f);
    glm::vec3 localAABBMax = glm::vec3(1e4f);
    glm::vec3 worldAABBMin = glm::vec3(-1e4f);
    glm::vec3 worldAABBMax = glm::vec3(1e4f);

    // Mesh optimization fields (used by SceneOptimizer)
    Uint32 vertexOffset = 0;
    Uint32 indexOffset = 0;
    Uint32 vertexCount = 0;
    Uint32 indexCount = 0;
    bool isGeometryDirty = true;

    // Renderer-assigned ID (assigned during registration)
    Uint32 rendererMeshId = UINT32_MAX;

    // Native Metal renderer bookkeeping (main's data model). The RHI renderer
    // uses renderMeshId/renderMaterialId instead and ignores these.
    Uint32 materialID = UINT32_MAX;
    Uint32 instanceID = UINT32_MAX;
};

} // namespace Vapor

// Forward declarations for RHI types (defined in rhi.hpp)
namespace Vapor {
struct BufferHandle;
struct TextureHandle;
struct PipelineHandle;
}