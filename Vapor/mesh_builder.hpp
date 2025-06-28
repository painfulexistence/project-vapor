#pragma once
#include <array>
#include <memory>
#include <SDL3/SDL_stdinc.h>

#include "graphics.hpp"

class MeshBuilder {
public:
    static std::shared_ptr<Mesh> buildTriforce(std::shared_ptr<Material> material = nullptr) {
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
        mesh->hasPosition = true;
        mesh->hasUV0 = true;
        mesh->primitiveMode = PrimitiveMode::TRIANGLES;
        mesh->initialize(verts, 6, indices, 6);

        return mesh;
    };

    static std::shared_ptr<Mesh> buildCube(float size, std::shared_ptr<Material> material = nullptr) {
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
        mesh->hasPosition = true;
        mesh->hasUV0 = true;
        mesh->hasNormal = true;
        mesh->primitiveMode = PrimitiveMode::TRIANGLES;
        mesh->initialize(verts.data(), verts.size(), tris.data(), tris.size());

        if (material) {
            mesh->material = material;
        }

        return mesh;
    };

    static std::shared_ptr<Mesh> buildCapsule(float size, std::shared_ptr<Material> material = nullptr) {
        auto mesh = std::make_shared<Mesh>();
        return mesh;
    };

    static std::shared_ptr<Mesh> buildCone(float size, std::shared_ptr<Material> material = nullptr) {
        auto mesh = std::make_shared<Mesh>();
        return mesh;
    };

    static std::shared_ptr<Mesh> buildCylinder(float size, std::shared_ptr<Material> material = nullptr) {
        auto mesh = std::make_shared<Mesh>();
        return mesh;
    };

private:
    MeshBuilder() = delete;
};