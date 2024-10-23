#include "mesh.hpp"
#include <utility>

void Mesh::initialize(const MeshData& data) {
    vertices = std::move(data.vertices);
    indices = std::move(data.indices);
};

void Mesh::initialize(VertexData* vertices, uint16_t* indices){

};

void Mesh::recalculateNormals(){

};

void Mesh::recalculateTangents(){

};