#pragma once
#include <vector>

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

struct VertexData {
    glm::vec3 position;
    glm::vec3 normal;
    glm::vec2 uv;
};

struct MeshData {
    std::vector<VertexData> vertices;
    std::vector<uint16_t> indices;
};

class Mesh {
public:
    void initialize(const MeshData& data);
    void initialize(VertexData* vertices, uint16_t* indices);
    void recalculateNormals();
    void recalculateTangents();

    std::vector<VertexData> vertices;
    std::vector<uint16_t> indices;
};

class MeshBuilder {
public:
    static Mesh* buildTriforce() {
        auto mesh = new Mesh();
        return mesh;
    };

    static Mesh* buildCube(float size) {
        auto mesh = new Mesh();
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