#include "graphics.hpp"
#include <utility>
#include <fmt/core.h>

void Mesh::initialize(const MeshData& data) {
    vertices = std::move(data.vertices);
    indices = std::move(data.indices);
    // recalculateNormals();
    recalculateTangents();
};

void Mesh::initialize(VertexData* vertexData, size_t vertexCount, Uint32* indexData, size_t indexCount){
    vertices.resize(vertexCount);
    for (size_t i = 0; i < vertexCount; ++i) {
        vertices[i] = vertexData[i];
    }
    indices.resize(indexCount);
    for (size_t i = 0; i < indexCount; ++i) {
        indices[i] = indexData[i];
    }
    // recalculateNormals();
    recalculateTangents();
};

void Mesh::recalculateNormals(){
    for (size_t i = 0; i < indices.size(); i += 3) {
        glm::vec3 edge1 = vertices[indices[i]].position - vertices[indices[i + 1]].position;
        glm::vec3 edge2 = vertices[indices[i + 2]].position - vertices[indices[i + 1]].position;
        // FIXME: may be wrong and need smoothing
        glm::vec3 normal = glm::normalize(glm::cross(edge2, edge1));
        vertices[indices[i]].normal = normal;
        vertices[indices[i + 1]].normal = normal;
        vertices[indices[i + 2]].normal = normal;
    }
};

void Mesh::recalculateTangents(){
    for (size_t i = 0; i < indices.size(); i += 3) {
        glm::vec3 edge1 = vertices[indices[i]].position - vertices[indices[i + 1]].position;
        glm::vec3 edge2 = vertices[indices[i + 2]].position - vertices[indices[i + 1]].position;
        glm::vec2 dUV1 = vertices[indices[i]].uv - vertices[indices[i + 1]].uv;
        glm::vec2 dUV2 = vertices[indices[i + 2]].uv - vertices[indices[i + 1]].uv;
        float f = 1.0f / (dUV1.x * dUV2.y - dUV1.y * dUV2.x);
        glm::vec3 tangent = f * (dUV2.y * edge1 - dUV1.y * edge2);
        vertices[indices[i]].tangent = tangent;
        vertices[indices[i + 1]].tangent = tangent;
        vertices[indices[i + 2]].tangent = tangent;
        // glm::vec3 bitangent = f * (dUV1.x * edge2 - dUV2.x * edge1);
        vertices[indices[i]].bitangent = glm::normalize(glm::cross(vertices[indices[i]].normal, tangent));
        vertices[indices[i + 1]].bitangent = glm::normalize(glm::cross(vertices[indices[i + 1]].normal, tangent));
        vertices[indices[i + 2]].bitangent = glm::normalize(glm::cross(vertices[indices[i + 2]].normal, tangent));
    }
}

void Mesh::print() {
    for (size_t i = 0; i < vertices.size(); i++) {
        fmt::print("Vertex {}: {} {} {}\n", i, vertices[i].position.x, vertices[i].position.y, vertices[i].position.z);
        fmt::print("Normal {}: {} {} {}\n", i, vertices[i].normal.x, vertices[i].normal.y, vertices[i].normal.z);
        fmt::print("Tangent {}: {} {} {}\n", i, vertices[i].tangent.x, vertices[i].tangent.y, vertices[i].tangent.z);
        fmt::print("Bitangent {}: {} {} {}\n", i, vertices[i].bitangent.x, vertices[i].bitangent.y, vertices[i].bitangent.z);
    }
}