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
    Uint32 rid = 0;
};

struct BufferHandle {
    Uint32 rid = 0;
};

struct TextureHandle {
    Uint32 rid = 0;
};

struct RenderTargetHandle {
    Uint32 rid = 0;
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

struct SceneData {
    float time;
};

struct alignas(16) CameraData {
    glm::mat4 proj;
    glm::mat4 view;
    glm::mat4 invProj;
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
    void recalculateNormalsAndTangents();
    void print();

    std::vector<BufferHandle> vbos;
    BufferHandle ebo;
    size_t bufferSize = 0;
    size_t vertexCount = 0;
    size_t indexCount = 0;
    std::vector<VertexData> vertices; // interleaved vertex data
    std::vector<glm::vec3> positions;
    std::vector<glm::vec3> normals;
    std::vector<glm::vec2> uv0s;
    std::vector<glm::vec2> uv1s;
    std::vector<glm::vec3> tangents;
    std::vector<glm::vec4> colors;
    std::vector<Uint32> indices;
    std::shared_ptr<Material> material = nullptr;
    PrimitiveMode primitiveMode;
};

class MeshBuilder {
public:
    static std::shared_ptr<Mesh> buildTriforce() {
        // glm::vec3 verts[6] = {
        //     { -0.5f, 0.5f, 0.0f }, { -0.5f, -0.5f, 0.0f }, { 0.5f, 0.5f, 0.0 },
        //     { 0.5f, 0.5f, 0.0f },  { -0.5f, -0.5f, 0.0f }, { 0.5f, -0.5f, 0.0f }
        // };
        // glm::vec2 uvs[6] = {
        //     { 0.0f, 0.0f }, { 0.0f, 1.0f }, { 1.0f, 0.0f }, { 1.0f, 0.0f }, { 0.0f, 1.0f }, { 1.0f, 1.0f }
        // };
        VertexData verts[6] = {
            { { -0.5f, 0.5f, 0.0f }, { 0.0f, 0.0f } },
            { { -0.5f, -0.5f, 0.0f }, { 0.0f, 1.0f } },
            { { 0.5f, 0.5f, 0.0f }, { 1.0f, 0.0f } },
            { { 0.5f, 0.5f, 0.0f }, { 1.0f, 0.0f } },
            { { -0.5f, -0.5f, 0.0f }, { 0.0f, 1.0f } },
            { { 0.5f, -0.5f, 0.0f }, { 1.0f, 1.0f } }
        };
        Uint32 indices[6] = { 0,  1,  2,  3,  4,  5 };

        auto mesh = std::make_shared<Mesh>();
        mesh->initialize(verts, 6, indices, 6);

        return mesh;
    };

    static std::shared_ptr<Mesh> buildCube(float size) {
        // float verts[192] = { // left
        //                     .5f,
        //                     .5f,
        //                     .5f,
        //                     0.0f,
        //                     0.0f,
        //                     1.0f,
        //                     1.0f,
        //                     1.0f,
        //                     .5f,
        //                     -.5f,
        //                     .5f,
        //                     0.0f,
        //                     0.0f,
        //                     1.0f,
        //                     1.0f,
        //                     0.0f,
        //                     -.5f,
        //                     .5f,
        //                     .5f,
        //                     0.0f,
        //                     0.0f,
        //                     1.0f,
        //                     0.0f,
        //                     1.0f,
        //                     -.5f,
        //                     -.5f,
        //                     .5f,
        //                     0.0f,
        //                     0.0f,
        //                     1.0f,
        //                     0.0f,
        //                     0.0f,
        //                     // right
        //                     .5f,
        //                     .5f,
        //                     -.5f,
        //                     0.0f,
        //                     0.0f,
        //                     -1.0f,
        //                     1.0f,
        //                     1.0f,
        //                     .5f,
        //                     -.5f,
        //                     -.5f,
        //                     0.0f,
        //                     0.0f,
        //                     -1.0f,
        //                     1.0f,
        //                     0.0f,
        //                     -.5f,
        //                     .5f,
        //                     -.5f,
        //                     0.0f,
        //                     0.0f,
        //                     -1.0f,
        //                     0.0f,
        //                     1.0f,
        //                     -.5f,
        //                     -.5f,
        //                     -.5f,
        //                     0.0f,
        //                     0.0f,
        //                     -1.0f,
        //                     0.0f,
        //                     0.0f,
        //                     // back
        //                     -.5f,
        //                     .5f,
        //                     .5f,
        //                     -1.0f,
        //                     0.0f,
        //                     0.0f,
        //                     1.0f,
        //                     1.0f,
        //                     -.5f,
        //                     .5f,
        //                     -.5f,
        //                     -1.0f,
        //                     0.0f,
        //                     0.0f,
        //                     0.0f,
        //                     1.0f,
        //                     -.5f,
        //                     -.5f,
        //                     .5f,
        //                     -1.0f,
        //                     0.0f,
        //                     0.0f,
        //                     1.0f,
        //                     0.0f,
        //                     -.5f,
        //                     -.5f,
        //                     -.5f,
        //                     -1.0f,
        //                     0.0f,
        //                     0.0f,
        //                     0.0f,
        //                     0.0f,
        //                     // front
        //                     .5f,
        //                     .5f,
        //                     .5f,
        //                     1.0f,
        //                     0.0f,
        //                     0.0f,
        //                     1.0f,
        //                     1.0f,
        //                     .5f,
        //                     .5f,
        //                     -.5f,
        //                     1.0f,
        //                     0.0f,
        //                     0.0f,
        //                     0.0f,
        //                     1.0f,
        //                     .5f,
        //                     -.5f,
        //                     .5f,
        //                     1.0f,
        //                     0.0f,
        //                     0.0f,
        //                     1.0f,
        //                     0.0f,
        //                     .5f,
        //                     -.5f,
        //                     -.5f,
        //                     1.0f,
        //                     0.0f,
        //                     0.0f,
        //                     0.0f,
        //                     0.0f,
        //                     // top
        //                     .5f,
        //                     .5f,
        //                     .5f,
        //                     0.0f,
        //                     1.0f,
        //                     0.0f,
        //                     1.0f,
        //                     1.0f,
        //                     .5f,
        //                     .5f,
        //                     -.5f,
        //                     0.0f,
        //                     1.0f,
        //                     0.0f,
        //                     1.0f,
        //                     0.0f,
        //                     -.5f,
        //                     .5f,
        //                     .5f,
        //                     0.0f,
        //                     1.0f,
        //                     0.0f,
        //                     0.0f,
        //                     1.0f,
        //                     -.5f,
        //                     .5f,
        //                     -.5f,
        //                     0.0f,
        //                     1.0f,
        //                     0.0f,
        //                     0.0f,
        //                     0.0f,
        //                     // bottom
        //                     .5f,
        //                     -.5f,
        //                     .5f,
        //                     0.0f,
        //                     -1.0f,
        //                     0.0f,
        //                     1.0f,
        //                     1.0f,
        //                     .5f,
        //                     -.5f,
        //                     -.5f,
        //                     0.0f,
        //                     -1.0f,
        //                     0.0f,
        //                     1.0f,
        //                     0.0f,
        //                     -.5f,
        //                     -.5f,
        //                     .5f,
        //                     0.0f,
        //                     -1.0f,
        //                     0.0f,
        //                     0.0f,
        //                     1.0f,
        //                     -.5f,
        //                     -.5f,
        //                     -.5f,
        //                     0.0f,
        //                     -1.0f,
        //                     0.0f,
        //                     0.0f,
        //                     0.0f
        // };
        // uint16_t tris[36] = { 0,  2,  1,  1,  2,  3,  4,  5,  6,  6,  5,  7,  8,  9,  10, 10, 9,  11,
        //                         12, 14, 13, 13, 14, 15, 16, 17, 18, 18, 17, 19, 20, 22, 21, 21, 22, 23 };
        // std::array<VertexData, 24> verts = {{
        //     // left
        //     { {.5f, .5f, .5f}, {1.0f, 1.0f}, {0.0f, 0.0f, 1.0f} },
        //     { {.5f, -.5f, .5f}, {1.0f, 0.0f}, {0.0f, 0.0f, 1.0f} },
        //     { {-.5f, .5f, .5f}, {0.0f, 1.0f}, {0.0f, 0.0f, 1.0f} },
        //     { {-.5f, -.5f, .5f}, {0.0f, 0.0f}, {0.0f, 0.0f, 1.0f} },
        //     // right
        //     { {.5f, .5f, -.5f}, {1.0f, 1.0f}, {0.0f, 0.0f, -1.0f} },
        //     { {.5f, -.5f, -.5f}, {1.0f, 0.0f}, {0.0f, 0.0f, -1.0f} },
        //     { {-.5f, .5f, -.5f}, {0.0f, 1.0f}, {0.0f, 0.0f, -1.0f} },
        //     { {-.5f, -.5f, -.5f}, {0.0f, 0.0f}, {0.0f, 0.0f, -1.0f} },
        //     // back
        //     { {-.5f, .5f, .5f}, {1.0f, 1.0f}, {1.0f, 0.0f, 0.0f} },
        //     { {-.5f, -.5f, .5f}, {1.0f, 0.0f}, {1.0f, 0.0f, 0.0f} },
        //     { {-.5f, .5f, -.5f}, {0.0f, 1.0f}, {1.0f, 0.0f, 0.0f} },
        //     { {-.5f, -.5f, -.5f}, {0.0f, 0.0f}, {1.0f, 0.0f, 0.0f} },
        //     // front
        //     { {.5f, .5f, .5f}, {1.0f, 1.0f}, {-1.0f, 0.0f, 0.0f} },
        //     { {.5f, .5f, -.5f}, {0.0f, 1.0f}, {-1.0f, 0.0f, 0.0f} },
        //     { {.5f, -.5f, .5f}, {1.0f, 0.0f}, {-1.0f, 0.0f, 0.0f} },
        //     { {.5f, -.5f, -.5f}, {0.0f, 0.0f}, {-1.0f, 0.0f, 0.0f} },
        //     // top
        //     { {.5f, .5f, .5f}, {1.0f, 1.0f}, {0.0f, 1.0f, 0.0f} },
        //     { {.5f, .5f, -.5f}, {1.0f, 0.0f}, {0.0f, 1.0f, 0.0f} },
        //     { {-.5f, .5f, .5f}, {0.0f, 1.0f}, {0.0f, 1.0f, 0.0f} },
        //     { {-.5f, .5f, -.5f}, {0.0f, 0.0f}, {0.0f, 1.0f, 0.0f} },
        //     // bottom
        //     { {.5f, -.5f, .5f}, {1.0f, 1.0f}, {0.0f, -1.0f, 0.0f} },
        //     { {.5f, -.5f, -.5f}, {1.0f, 0.0f}, {0.0f, -1.0f, 0.0f} },
        //     { {-.5f, -.5f, .5f}, {0.0f, 1.0f}, {0.0f, -1.0f, 0.0f} },
        //     { {-.5f, -.5f, -.5f}, {0.0f, 0.0f}, {0.0f, -1.0f, 0.0f} }
        // }};

        // std::array<uint16_t, 36> tris = { 0,  2,  1,  1,  2,  3,  4,  5,  6,  6,  5,  7,  8,  9,  10, 10, 9,  11,
        //                         12, 14, 13, 13, 14, 15, 16, 17, 18, 18, 17, 19, 20, 22, 21, 21, 22, 23 };
        std::array<VertexData, 24> verts = {{
            // front
            { { .5f * size, .5f * size, .5f * size }, { 1.f, 1.f }, { 0.0f, 0.0f, 1.0f } },
            { { -.5f * size, .5f * size, .5f * size }, { 0.f, 1.f }, { 0.0f, 0.0f, 1.0f } },
            { { .5f * size, -.5f * size, .5f * size }, { 1.f, 0.f }, { 0.0f, 0.0f, 1.0f } },
            { { -.5f * size, -.5f * size, .5f * size }, { 0.f, 0.f }, { 0.0f, 0.0f, 1.0f } },
            // back
            { { -.5f * size, .5f * size, -.5f * size }, { 1.f, 1.f }, { 0.0f, 0.0f, -1.0f } },
            { { .5f * size, .5f * size, -.5f * size }, { 0.f, 1.f }, { 0.0f, 0.0f, -1.0f } },
            { { -.5f * size, -.5f * size, -.5f * size }, { 1.f, 0.f }, { 0.0f, 0.0f, -1.0f } },
            { { .5f * size, -.5f * size, -.5f * size }, { 0.f, 0.f }, { 0.0f, 0.0f, -1.0f } },
            // right
            { { .5f * size, .5f * size, -.5f * size }, { 1.f, 1.f }, { 1.0f, 0.0f, 0.0f } },
            { { .5f * size, .5f * size, .5f * size }, { 0.f, 1.f }, { 1.0f, 0.0f, 0.0f } },
            { { .5f * size, -.5f * size, -.5f * size }, { 1.f, 0.f }, { 1.0f, 0.0f, 0.0f } },
            { { .5f * size, -.5f * size, .5f * size }, { 0.f, 0.f }, { 1.0f, 0.0f, 0.0f } },
            // left
            { { -.5f * size, .5f * size, .5f * size }, { 1.f, 1.f }, { -1.0f, 0.0f, 0.0f } },
            { { -.5f * size, .5f * size, -.5f * size }, { 0.f, 1.f }, { -1.0f, 0.0f, 0.0f } },
            { { -.5f * size, -.5f * size, .5f * size }, { 1.f, 0.f }, { -1.0f, 0.0f, 0.0f } },
            { { -.5f * size, -.5f * size, -.5f * size }, { 0.f, 0.f }, { -1.0f, 0.0f, 0.0f } },
            // top
            { { .5f * size, .5f * size, -.5f * size }, { 1.f, 1.f }, { 0.0f, 1.0f, 0.0f } },
            { { -.5f * size, .5f * size, -.5f * size }, { 0.f, 1.f }, { 0.0f, 1.0f, 0.0f } },
            { { .5f * size, .5f * size, .5f * size }, { 1.f, 0.f }, { 0.0f, 1.0f, 0.0f } },
            { { -.5f * size, .5f * size, .5f * size }, { 0.f, 0.f }, { 0.0f, 1.0f, 0.0f } },
            // bottom
            { { .5f * size, -.5f * size, .5f * size }, { 1.0f, 1.0f }, { 0.0f, -1.0f, 0.0f } },
            { { -.5f * size, -.5f * size, .5f * size }, { 0.0f, 1.0f }, { 0.0f, -1.0f, 0.0f } },
            { { .5f * size, -.5f * size, -.5f * size }, { 1.0f, 0.0f }, { 0.0f, -1.0f, 0.0f } },
            { { -.5f * size, -.5f * size, -.5f * size }, { 0.0f, 0.0f }, { 0.0f, -1.0f, 0.0f } }
        }};
        std::array<Uint32, 36> tris = {
            0, 1, 2,
            2, 1, 3,
            4, 5, 6,
            6, 5, 7,
            8, 9, 10,
            10, 9, 11,
            12, 13, 14,
            14, 13, 15,
            16, 17, 18,
            18, 17, 19,
            20, 21, 22,
            22, 21, 23
        };
        auto mesh = std::make_shared<Mesh>();
        mesh->initialize(verts.data(), verts.size(), tris.data(), tris.size());

        return mesh;
    };

    static std::shared_ptr<Mesh> buildCapsule(float size) {
        auto mesh = std::make_shared<Mesh>();
        return mesh;
    };

    static std::shared_ptr<Mesh> buildCone(float size) {
        auto mesh = std::make_shared<Mesh>();
        return mesh;
    };

    static std::shared_ptr<Mesh> buildCylinder(float size) {
        auto mesh = std::make_shared<Mesh>();
        return mesh;
    };

private:
    MeshBuilder() = delete;
};