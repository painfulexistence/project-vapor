#include "graphics.hpp"
#include <fmt/core.h>
#include "MikkTSpace/mikktspace.h"
#include <utility>
#include <limits>

static int getNumFaces(const SMikkTSpaceContext* ctx) {
    auto mesh = static_cast<Mesh*>(ctx->m_pUserData);
    if (mesh->indices.size() == 0) {
        return mesh->vertices.size() / 3;
    }
    return mesh->indices.size() / 3;
}

static int getNumVerticesOfFace(const SMikkTSpaceContext* ctx, const int face) {
    return 3;
}

static void getPosition(const SMikkTSpaceContext* ctx, float* pos, const int face, const int vert) {
    auto mesh = static_cast<Mesh*>(ctx->m_pUserData);
    auto index = mesh->indices.size() == 0 ? face * 3 + vert : mesh->indices[face * 3 + vert];
    pos[0] = mesh->vertices[index].position.x;
    pos[1] = mesh->vertices[index].position.y;
    pos[2] = mesh->vertices[index].position.z;
}

static void getNormal(const SMikkTSpaceContext* ctx, float* normal, const int face, const int vert) {
    auto mesh = static_cast<Mesh*>(ctx->m_pUserData);
    auto index = mesh->indices.size() == 0 ? face * 3 + vert : mesh->indices[face * 3 + vert];
    normal[0] = mesh->vertices[index].normal.x;
    normal[1] = mesh->vertices[index].normal.y;
    normal[2] = mesh->vertices[index].normal.z;
}

static void getTexCoord(const SMikkTSpaceContext* ctx, float* texC, const int face, const int vert) {
    auto mesh = static_cast<Mesh*>(ctx->m_pUserData);
    auto index = mesh->indices.size() == 0 ? face * 3 + vert : mesh->indices[face * 3 + vert];
    texC[0] = mesh->vertices[index].uv.x;
    texC[1] = mesh->vertices[index].uv.y;
}

static void setTSpaceBasic(const SMikkTSpaceContext* ctx, const float* tan, float sign, const int face, const int vert) {
    auto mesh = static_cast<Mesh*>(ctx->m_pUserData);
    auto index = mesh->indices.size() == 0 ? face * 3 + vert : mesh->indices[face * 3 + vert];
    mesh->vertices[index].tangent = glm::vec4(tan[0], tan[1], tan[2], sign);
}

void Mesh::initialize(const std::vector<VertexData>& vertices, const std::vector<Uint32>& indices) {
    this->vertices = std::move(vertices);
    this->indices = std::move(indices);
    // recalculateNormals();
    calculateTangents();
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
    // calculateNormals();
    calculateTangents();
};

void Mesh::calculateNormals(){
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

void Mesh::calculateTangents(){
    SMikkTSpaceInterface interface {
        .m_getNumFaces = getNumFaces,
        .m_getNumVerticesOfFace = getNumVerticesOfFace,
        .m_getPosition = getPosition,
        .m_getNormal = getNormal,
        .m_getTexCoord = getTexCoord,
        .m_setTSpaceBasic = setTSpaceBasic,
    };
    SMikkTSpaceContext ctx {
        .m_pInterface = &interface,
        .m_pUserData = this
    };
    if (!genTangSpaceDefault(&ctx)) {
        throw std::runtime_error("Mikktspace calculation failed");
    }
}

void Mesh::calculateLocalAABB(){
    localAABBMin = glm::vec3(std::numeric_limits<float>::max());
    localAABBMax = glm::vec3(-std::numeric_limits<float>::max());
    for (const auto& vertex : vertices) {
        localAABBMin = glm::min(localAABBMin, vertex.position);
        localAABBMax = glm::max(localAABBMax, vertex.position);
    }
};

glm::vec4 Mesh::getWorldBoundingSphere() const {
    glm::vec3 center = (worldAABBMin + worldAABBMax) * 0.5f;
    float radius = glm::length(worldAABBMax - center);
    return glm::vec4(center, radius);
}

void Mesh::print() {
    for (size_t i = 0; i < vertices.size(); i++) {
        fmt::print("Vertex {}: {} {} {}\n", i, vertices[i].position.x, vertices[i].position.y, vertices[i].position.z);
        fmt::print("Normal {}: {} {} {}\n", i, vertices[i].normal.x, vertices[i].normal.y, vertices[i].normal.z);
        fmt::print("Tangent {}: {} {} {}\n", i, vertices[i].tangent.x, vertices[i].tangent.y, vertices[i].tangent.z);
        // fmt::print("Bitangent {}: {} {} {}\n", i, vertices[i].bitangent.x, vertices[i].bitangent.y, vertices[i].bitangent.z);
    }
}