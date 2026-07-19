#pragma once
#include "rhi.hpp"  // TextureHandle (value member on Image; used by the native Metal renderer)
#include "meshlet.hpp"  // MeshletData (baked meshlet + cluster-LOD data on Mesh)
#include "graphics_gpu_structs.hpp"  // GPU upload structs (MaterialData, lights, InstanceData, …)
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
    // KHR_materials_transmission factor. RENDERING support only for now: the
    // glTF importer does not parse the extension yet (separate PR); set from
    // code / the Scene Materials panel. IOR is fixed at 1.5 (the glTF default).
    float transmission = 0.0f;

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