#pragma once
#include <SDL3/SDL_stdinc.h>
#include <glm/geometric.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>
#include <glm/mat4x4.hpp>
#include <cstddef>
#include <vector>
#include <array>

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
    glm::vec4 baseColorFactor;
    float normalScale;
    float metallicFactor;
    float roughnessFactor;
    float occlusionStrength;
    glm::vec3 emissiveFactor;
    std::shared_ptr<Image> albedoMap;
    std::shared_ptr<Image> normalMap;
    std::shared_ptr<Image> metallicRoughnessMap;
    std::shared_ptr<Image> occlusionMap;
    std::shared_ptr<Image> emissiveMap;
    std::shared_ptr<Image> displacementMap;
    PipelineHandle pipeline;
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
};

struct alignas(16) InstanceData {
    glm::mat4 model;
    glm::vec4 color;
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
    glm::vec3 tangent;
    glm::vec3 bitangent;
};

struct MeshData {
    std::vector<VertexData> vertices;
    std::vector<Uint32> indices;
};

struct Mesh {
    void initialize(const MeshData& data);
    void initialize(VertexData* vertexData, size_t vertexCount, Uint32* indexData, size_t indexCount);
    void recalculateNormals();
    void recalculateTangents();
    void print();

    std::vector<BufferHandle> vbos;
    BufferHandle ebo;
    size_t bufferSize = 0;
    size_t vertexCount = 0;
    size_t indexCount = 0;
    bool hasPosition = false;
    bool hasNormal = false;
    bool hasTangent = false;
    bool hasUV0 = false;
    bool hasUV1 = false;
    bool hasColor = false;
    std::vector<VertexData> vertices; // interleaved vertex data
    std::vector<Uint32> indices;
    std::shared_ptr<Material> material = nullptr;
    PrimitiveMode primitiveMode;
    Uint32 instanceID = UINT32_MAX; // also used as blas index
};