#pragma once
#include <cstddef>
#include <vector>
#include <array>

#include <glm/geometric.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

struct VertexData {
    glm::vec3 position;
    glm::vec2 uv;
    glm::vec3 normal;
    glm::vec3 tangent;
    glm::vec3 bitangent;
};

struct MeshData {
    std::vector<VertexData> vertices;
    std::vector<uint16_t> indices;
};

class Mesh {
public:
    void initialize(const MeshData& data);
    void initialize(VertexData* vertexData, size_t vertexCount, uint16_t* indexData, size_t indexCount);
    void recalculateNormalsAndTangents();

    std::vector<VertexData> vertices;
    std::vector<uint16_t> indices;
};

class MeshBuilder {
public:
    static Mesh* buildTriforce() {
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
        uint16_t indices[6] = { 0,  1,  2,  3,  4,  5 };

        auto mesh = new Mesh();
        mesh->initialize(verts, 6, indices, 6);

        return mesh;
    };

    static Mesh* buildCube(float size) {
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
        std::array<uint16_t, 36> tris = {
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
        auto mesh = new Mesh();
        mesh->initialize(verts.data(), verts.size(), tris.data(), tris.size());

        return mesh;
    };

    static Mesh* buildCapsule(float size) {
        auto mesh = new Mesh();
        return mesh;
    };

    static Mesh* buildCone(float size) {
        auto mesh = new Mesh();
        return mesh;
    };

    static Mesh* buildCylinder(float size) {
        auto mesh = new Mesh();
        return mesh;
    };

private:
    MeshBuilder() = delete;
};