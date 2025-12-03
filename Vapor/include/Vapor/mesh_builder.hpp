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

    // Build a capsule mesh (cylinder + 2 hemisphere caps)
    // height: total height of capsule (including both hemisphere caps)
    // radius: radius of capsule
    // segments: number of segments around the circumference
    // rings: number of rings in each hemisphere
    static std::shared_ptr<Mesh> buildCapsule(
        float height,
        float radius,
        Uint32 segments = 16,
        Uint32 rings = 8,
        std::shared_ptr<Material> material = nullptr
    ) {
        std::vector<VertexData> verts;
        std::vector<Uint32> indices;

        // Calculate cylinder height (total height - 2 hemisphere caps)
        float cylinderHeight = height - 2.0f * radius;
        float halfCylinderHeight = cylinderHeight * 0.5f;

        // Generate top hemisphere (from top to equator)
        for (Uint32 ring = 0; ring <= rings; ++ring) {
            float v = static_cast<float>(ring) / static_cast<float>(rings);
            float phi = v * (M_PI * 0.5f); // 0 to PI/2

            float y = halfCylinderHeight + radius * cosf(phi);
            float ringRadius = radius * sinf(phi);

            for (Uint32 seg = 0; seg <= segments; ++seg) {
                float u = static_cast<float>(seg) / static_cast<float>(segments);
                float theta = u * 2.0f * M_PI;

                float x = ringRadius * cosf(theta);
                float z = ringRadius * sinf(theta);

                glm::vec3 pos(x, y, z);
                glm::vec3 normal = glm::normalize(glm::vec3(x, y - halfCylinderHeight, z));
                glm::vec2 uv(u, v * 0.25f); // Top hemisphere uses 0-0.25 of V

                verts.push_back({ pos, uv, normal });
            }
        }

        Uint32 topHemisphereVertexCount = (rings + 1) * (segments + 1);

        // Generate cylinder body
        Uint32 cylinderRings = 2; // Top and bottom edge
        for (Uint32 ring = 0; ring < cylinderRings; ++ring) {
            float v = static_cast<float>(ring) / static_cast<float>(cylinderRings - 1);
            float y = halfCylinderHeight - v * cylinderHeight;

            for (Uint32 seg = 0; seg <= segments; ++seg) {
                float u = static_cast<float>(seg) / static_cast<float>(segments);
                float theta = u * 2.0f * M_PI;

                float x = radius * cosf(theta);
                float z = radius * sinf(theta);

                glm::vec3 pos(x, y, z);
                glm::vec3 normal = glm::normalize(glm::vec3(x, 0, z));
                glm::vec2 uv(u, 0.25f + v * 0.5f); // Cylinder uses 0.25-0.75 of V

                verts.push_back({ pos, uv, normal });
            }
        }

        Uint32 cylinderVertexCount = cylinderRings * (segments + 1);

        // Generate bottom hemisphere (from equator to bottom)
        for (Uint32 ring = 0; ring <= rings; ++ring) {
            float v = static_cast<float>(ring) / static_cast<float>(rings);
            float phi = v * (M_PI * 0.5f); // 0 to PI/2

            float y = -halfCylinderHeight - radius * sinf(phi);
            float ringRadius = radius * cosf(phi);

            for (Uint32 seg = 0; seg <= segments; ++seg) {
                float u = static_cast<float>(seg) / static_cast<float>(segments);
                float theta = u * 2.0f * M_PI;

                float x = ringRadius * cosf(theta);
                float z = ringRadius * sinf(theta);

                glm::vec3 pos(x, y, z);
                glm::vec3 normal = glm::normalize(glm::vec3(x, y + halfCylinderHeight, z));
                glm::vec2 uv(u, 0.75f + v * 0.25f); // Bottom hemisphere uses 0.75-1.0 of V

                verts.push_back({ pos, uv, normal });
            }
        }

        // Generate indices for top hemisphere
        for (Uint32 ring = 0; ring < rings; ++ring) {
            for (Uint32 seg = 0; seg < segments; ++seg) {
                Uint32 current = ring * (segments + 1) + seg;
                Uint32 next = current + segments + 1;

                indices.push_back(current);
                indices.push_back(next);
                indices.push_back(current + 1);

                indices.push_back(current + 1);
                indices.push_back(next);
                indices.push_back(next + 1);
            }
        }

        // Generate indices for cylinder body
        Uint32 cylinderStartVertex = topHemisphereVertexCount;
        for (Uint32 ring = 0; ring < cylinderRings - 1; ++ring) {
            for (Uint32 seg = 0; seg < segments; ++seg) {
                Uint32 current = cylinderStartVertex + ring * (segments + 1) + seg;
                Uint32 next = current + segments + 1;

                indices.push_back(current);
                indices.push_back(next);
                indices.push_back(current + 1);

                indices.push_back(current + 1);
                indices.push_back(next);
                indices.push_back(next + 1);
            }
        }

        // Generate indices for bottom hemisphere
        Uint32 bottomHemisphereStartVertex = topHemisphereVertexCount + cylinderVertexCount;
        for (Uint32 ring = 0; ring < rings; ++ring) {
            for (Uint32 seg = 0; seg < segments; ++seg) {
                Uint32 current = bottomHemisphereStartVertex + ring * (segments + 1) + seg;
                Uint32 next = current + segments + 1;

                indices.push_back(current);
                indices.push_back(next);
                indices.push_back(current + 1);

                indices.push_back(current + 1);
                indices.push_back(next);
                indices.push_back(next + 1);
            }
        }

        auto mesh = std::make_shared<Mesh>();
        mesh->hasPosition = true;
        mesh->hasUV0 = true;
        mesh->hasNormal = true;
        mesh->primitiveMode = PrimitiveMode::TRIANGLES;
        mesh->initialize(verts.data(), verts.size(), indices.data(), indices.size());

        if (material) {
            mesh->material = material;
        }

        return mesh;
    };

    static std::shared_ptr<Mesh> buildCone(float size, std::shared_ptr<Material> material = nullptr) {
        auto mesh = std::make_shared<Mesh>();
        return mesh;
    };

    // Build a cylinder mesh (with top and bottom caps)
    // height: total height of cylinder
    // radius: radius of cylinder
    // segments: number of segments around the circumference
    static std::shared_ptr<Mesh> buildCylinder(
        float height,
        float radius,
        Uint32 segments = 16,
        std::shared_ptr<Material> material = nullptr
    ) {
        std::vector<VertexData> verts;
        std::vector<Uint32> indices;

        float halfHeight = height * 0.5f;

        // Generate side vertices (2 rings: top and bottom)
        for (Uint32 ring = 0; ring < 2; ++ring) {
            float y = (ring == 0) ? halfHeight : -halfHeight;
            float v = static_cast<float>(ring);

            for (Uint32 seg = 0; seg <= segments; ++seg) {
                float u = static_cast<float>(seg) / static_cast<float>(segments);
                float theta = u * 2.0f * M_PI;

                float x = radius * cosf(theta);
                float z = radius * sinf(theta);

                glm::vec3 pos(x, y, z);
                glm::vec3 normal = glm::normalize(glm::vec3(x, 0, z));
                glm::vec2 uv(u, v);

                verts.push_back({ pos, uv, normal });
            }
        }

        // Generate side indices
        for (Uint32 seg = 0; seg < segments; ++seg) {
            Uint32 current = seg;
            Uint32 next = current + segments + 1;

            indices.push_back(current);
            indices.push_back(next);
            indices.push_back(current + 1);

            indices.push_back(current + 1);
            indices.push_back(next);
            indices.push_back(next + 1);
        }

        Uint32 sideVertexCount = 2 * (segments + 1);

        // Generate top cap
        Uint32 topCenterIndex = static_cast<Uint32>(verts.size());
        verts.push_back({ glm::vec3(0, halfHeight, 0), glm::vec2(0.5f, 0.5f), glm::vec3(0, 1, 0) });

        for (Uint32 seg = 0; seg <= segments; ++seg) {
            float u = static_cast<float>(seg) / static_cast<float>(segments);
            float theta = u * 2.0f * M_PI;

            float x = radius * cosf(theta);
            float z = radius * sinf(theta);

            glm::vec3 pos(x, halfHeight, z);
            glm::vec2 uv(0.5f + 0.5f * cosf(theta), 0.5f + 0.5f * sinf(theta));
            glm::vec3 normal(0, 1, 0);

            verts.push_back({ pos, uv, normal });
        }

        // Top cap indices
        for (Uint32 seg = 0; seg < segments; ++seg) {
            indices.push_back(topCenterIndex);
            indices.push_back(topCenterIndex + seg + 1);
            indices.push_back(topCenterIndex + seg + 2);
        }

        // Generate bottom cap
        Uint32 bottomCenterIndex = static_cast<Uint32>(verts.size());
        verts.push_back({ glm::vec3(0, -halfHeight, 0), glm::vec2(0.5f, 0.5f), glm::vec3(0, -1, 0) });

        for (Uint32 seg = 0; seg <= segments; ++seg) {
            float u = static_cast<float>(seg) / static_cast<float>(segments);
            float theta = u * 2.0f * M_PI;

            float x = radius * cosf(theta);
            float z = radius * sinf(theta);

            glm::vec3 pos(x, -halfHeight, z);
            glm::vec2 uv(0.5f + 0.5f * cosf(theta), 0.5f + 0.5f * sinf(theta));
            glm::vec3 normal(0, -1, 0);

            verts.push_back({ pos, uv, normal });
        }

        // Bottom cap indices (reversed winding for correct face orientation)
        for (Uint32 seg = 0; seg < segments; ++seg) {
            indices.push_back(bottomCenterIndex);
            indices.push_back(bottomCenterIndex + seg + 2);
            indices.push_back(bottomCenterIndex + seg + 1);
        }

        auto mesh = std::make_shared<Mesh>();
        mesh->hasPosition = true;
        mesh->hasUV0 = true;
        mesh->hasNormal = true;
        mesh->primitiveMode = PrimitiveMode::TRIANGLES;
        mesh->initialize(verts.data(), verts.size(), indices.data(), indices.size());

        if (material) {
            mesh->material = material;
        }

        return mesh;
    };

    // Build a water grid mesh with two UV channels:
    // - uv0: tiled coordinates for normal map scrolling
    // - uv1: whole grid coordinates (0-1) for edge dampening
    // gridSize: number of tiles in X and Z directions
    // tileSize: world space size of each tile
    // texTile: UV tiling factor for normal maps
    static void buildWaterGrid(
        Uint32 gridSizeX, Uint32 gridSizeZ,
        float tileSize,
        float texTileX, float texTileZ,
        std::vector<WaterVertexData>& outVertices,
        std::vector<Uint32>& outIndices
    ) {
        outVertices.clear();
        outIndices.clear();

        // Total size of the grid
        float totalSizeX = gridSizeX * tileSize;
        float totalSizeZ = gridSizeZ * tileSize;

        // Center the grid
        float offsetX = -totalSizeX * 0.5f;
        float offsetZ = -totalSizeZ * 0.5f;

        float oneOverXTiles = 1.0f / static_cast<float>(gridSizeX);
        float oneOverZTiles = 1.0f / static_cast<float>(gridSizeZ);

        // Create vertices for each tile (6 vertices per tile for 2 triangles)
        for (Uint32 x = 0; x < gridSizeX; ++x) {
            for (Uint32 z = 0; z < gridSizeZ; ++z) {
                // Tiled UV coordinates
                float xBeginTile = (oneOverXTiles * static_cast<float>(x)) * texTileX;
                float xEndTile = (oneOverXTiles * static_cast<float>(x + 1)) * texTileX;
                float zBeginTile = (oneOverZTiles * static_cast<float>(z)) * texTileZ;
                float zEndTile = (oneOverZTiles * static_cast<float>(z + 1)) * texTileZ;

                // Whole grid UV coordinates (0-1)
                float xBegin = oneOverXTiles * static_cast<float>(x);
                float xEnd = oneOverXTiles * static_cast<float>(x + 1);
                float zBegin = oneOverZTiles * static_cast<float>(z);
                float zEnd = oneOverZTiles * static_cast<float>(z + 1);

                // World positions
                float posX0 = offsetX + x * tileSize;
                float posX1 = offsetX + (x + 1) * tileSize;
                float posZ0 = offsetZ + z * tileSize;
                float posZ1 = offsetZ + (z + 1) * tileSize;

                Uint32 baseIndex = static_cast<Uint32>(outVertices.size());

                // Vertex 0: bottom-left
                outVertices.push_back({
                    { posX0, 0.0f, posZ0 },
                    { xBeginTile, zBeginTile },
                    { xBegin, zBegin }
                });

                // Vertex 1: bottom-right
                outVertices.push_back({
                    { posX1, 0.0f, posZ0 },
                    { xEndTile, zBeginTile },
                    { xEnd, zBegin }
                });

                // Vertex 2: top-left
                outVertices.push_back({
                    { posX0, 0.0f, posZ1 },
                    { xBeginTile, zEndTile },
                    { xBegin, zEnd }
                });

                // Vertex 3: top-right
                outVertices.push_back({
                    { posX1, 0.0f, posZ1 },
                    { xEndTile, zEndTile },
                    { xEnd, zEnd }
                });

                // Triangle 1: 0, 2, 1
                outIndices.push_back(baseIndex + 0);
                outIndices.push_back(baseIndex + 2);
                outIndices.push_back(baseIndex + 1);

                // Triangle 2: 1, 2, 3
                outIndices.push_back(baseIndex + 1);
                outIndices.push_back(baseIndex + 2);
                outIndices.push_back(baseIndex + 3);
            }
        }
    };

private:
    MeshBuilder() = delete;
};