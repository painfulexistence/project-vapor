#pragma once
// Mesh and material CPU-side data structures.
#include "graphics_handles.hpp"
#include "graphics_gpu_structs.hpp"
#include <SDL3/SDL_stdinc.h>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>
#include <memory>
#include <string>
#include <vector>

enum class AlphaMode { OPAQUE, MASK, BLEND };

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
    glm::vec4 baseColorFactor  = glm::vec4(1.0f);
    float normalScale          = 1.0f;
    float metallicFactor       = 1.0f;
    float roughnessFactor      = 1.0f;
    float occlusionStrength    = 1.0f;
    glm::vec3 emissiveFactor   = glm::vec3(0.0f);
    float emissiveStrength     = 1.0f;
    std::shared_ptr<Image> albedoMap;
    std::shared_ptr<Image> normalMap;
    std::shared_ptr<Image> metallicMap;
    std::shared_ptr<Image> roughnessMap;
    std::shared_ptr<Image> occlusionMap;
    std::shared_ptr<Image> emissiveMap;
    std::shared_ptr<Image> displacementMap;
    float subsurface           = 0.0f;
    float specular             = 0.5f;
    float specularTint         = 0.0f;
    float anisotropic          = 0.0f;
    float sheen                = 0.0f;
    float sheenTint            = 0.5f;
    float clearcoat            = 0.0f;
    float clearcoatGloss;
    // Prototype UV Mode: 0 = Off, 1 = World Space, 2 = Object Space
    int prototypeUVMode        = 0;
    float uvScale              = 1.0f;
    PipelineHandle pipeline;
};

struct VertexData {
    glm::vec3 position;
    glm::vec2 uv;
    glm::vec3 normal;
    glm::vec4 tangent;
};

// Water vertex — two UV channels: tiled (uv0) and whole-grid (uv1)
struct WaterVertexData {
    glm::vec3 position;
    glm::vec2 uv0;
    glm::vec2 uv1;
};

struct Mesh {
    void initialize(const std::vector<VertexData>& vertices, const std::vector<Uint32>& indices);
    void initialize(VertexData* vertexData, size_t vertexCount, Uint32* indexData, size_t indexCount);
    void calculateNormals();
    void calculateTangents();
    void calculateLocalAABB();
    glm::vec4 getWorldBoundingSphere() const;
    void print();

    bool hasPosition    = false;
    bool hasNormal      = false;
    bool hasTangent     = false;
    bool hasUV0         = false;
    bool hasUV1         = false;
    bool hasColor       = false;
    std::vector<VertexData> vertices;
    std::vector<Uint32> indices;
    std::shared_ptr<Material> material = nullptr;
    PrimitiveMode primitiveMode;
    glm::vec3 localAABBMin;
    glm::vec3 localAABBMax;
    glm::vec3 worldAABBMin;
    glm::vec3 worldAABBMax;
    bool isGeometryDirty = true;

    // GPU-driven rendering offsets into the scene's flat vertex/index buffers
    Uint32 vertexOffset = 0;
    Uint32 indexOffset  = 0;
    Uint32 vertexCount  = 0;
    Uint32 indexCount   = 0;

    std::vector<BufferHandle> vbos;
    BufferHandle ebo;
    Uint32 instanceID = UINT32_MAX;
    Uint32 materialID = UINT32_MAX;
};
