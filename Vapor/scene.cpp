#include "scene.hpp"

#include <SDL3/SDL_log.h>
#include <fmt/core.h>
#include <functional>

void Scene::print() {
    fmt::print("Scene {}\n", name);
    fmt::print(" Images: {}\n", images.size());
    fmt::print(" Materials: {}\n", materials.size());
    fmt::print(" Total vertices: {}, total indices: {}\n", vertices.size(), indices.size());
    fmt::print("--------------------------------\n");
    for (const auto& node : nodes) {
        printNode(node);
    }
}

void Scene::printNode(const std::shared_ptr<Node>& node) {
    fmt::print("  Node {}\n", node->name);
    fmt::print("--------------------------------\n");
    if (node->meshGroup) {
        fmt::print("   Mesh group {} ({} meshes)\n", node->meshGroup->name, node->meshGroup->meshes.size());
        for (const auto& mesh : node->meshGroup->meshes) {
            fmt::print("    Mesh\n");
            fmt::print("     vertexOffset={}, indexOffset={}, vertexCount={}, indexCount={}\n",
                mesh->vertexOffset, mesh->indexOffset, mesh->vertexCount, mesh->indexCount);
            fmt::print("     AABB: min=({}, {}, {}), max=({}, {}, {})\n",
                mesh->worldAABBMin.x, mesh->worldAABBMin.y, mesh->worldAABBMin.z,
                mesh->worldAABBMax.x, mesh->worldAABBMax.y, mesh->worldAABBMax.z);
            // SDL_Log("Vertex count: %zu", mesh->vertices.size());
            // if (mesh->indices.size() > 0) {
            //     SDL_Log("Index count: %zu", mesh->indices.size());
            //     for (const Uint32& idx : mesh->indices) {
            //         SDL_Log(
            //             "(Vertex %u) Position: %f, %f, %f, UV: %f, %f, Normal: %f, %f, %f",
            //             idx,
            //             mesh->vertices[idx].position.x,
            //             mesh->vertices[idx].position.y,
            //             mesh->vertices[idx].position.z,
            //             mesh->vertices[idx].uv.x,
            //             mesh->vertices[idx].uv.y,
            //             mesh->vertices[idx].normal.x,
            //             mesh->vertices[idx].normal.y,
            //             mesh->vertices[idx].normal.z
            //         );
            //     }
            // }
        }
    }
    fmt::print("--------------------------------\n");
    for (const auto& child : node->children) {
        printNode(child);
    }
}

std::shared_ptr<Node> Scene::createNode(const std::string& name, const glm::mat4& transform) {
    auto node = std::make_shared<Node>();
    node->name = name;
    node->localTransform = transform;
    nodes.push_back(node);
    return node;
}

void Scene::addNode(std::shared_ptr<Node> node) {
    nodes.push_back(node);
}

std::shared_ptr<Node> Scene::findNode(const std::string& name) {
    for (const auto& node : nodes) {
        auto result = findNodeInHierarchy(name, node);
        if (result) {
            return result;
        }
    }
    return nullptr;
}

std::shared_ptr<Node> Scene::findNodeInHierarchy(const std::string& name, const std::shared_ptr<Node>& node) {
    if (node->name == name) {
        return node;
    }
    for (const auto& childNode : node->children) {
        auto result = findNodeInHierarchy(name, childNode);
        if (result) {
            return result;
        }
    }
    return nullptr;
}

void Scene::update(float dt) {
    const glm::mat4 rootTransform = glm::scale(glm::mat4(1.0f), glm::vec3(1.0f));
    for (const auto& node : nodes) {
        updateNode(node, rootTransform);
    }
}

void Scene::updateNode(const std::shared_ptr<Node>& node, const glm::mat4& parentTransform) {
    if (node->isTransformDirty) {
        node->worldTransform = parentTransform * node->localTransform;
        if (node->meshGroup) {
            for (const auto& mesh : node->meshGroup->meshes) {
                // update local AABB if geometry is new or changed
                if (mesh->isGeometryDirty) {
                    mesh->calculateLocalAABB();
                    mesh->isGeometryDirty = false;
                }
                std::array<glm::vec3, 8> corners = {
                    glm::vec3(mesh->localAABBMin.x, mesh->localAABBMin.y, mesh->localAABBMin.z),
                    glm::vec3(mesh->localAABBMin.x, mesh->localAABBMin.y, mesh->localAABBMax.z),
                    glm::vec3(mesh->localAABBMin.x, mesh->localAABBMax.y, mesh->localAABBMin.z),
                    glm::vec3(mesh->localAABBMax.x, mesh->localAABBMin.y, mesh->localAABBMin.z),
                    glm::vec3(mesh->localAABBMin.x, mesh->localAABBMax.y, mesh->localAABBMax.z),
                    glm::vec3(mesh->localAABBMax.x, mesh->localAABBMin.y, mesh->localAABBMax.z),
                    glm::vec3(mesh->localAABBMax.x, mesh->localAABBMax.y, mesh->localAABBMin.z),
                    glm::vec3(mesh->localAABBMax.x, mesh->localAABBMax.y, mesh->localAABBMax.z)
                };
                mesh->worldAABBMin = glm::vec3(FLT_MAX);
                mesh->worldAABBMax = glm::vec3(-FLT_MAX);
                for (const auto& corner : corners) {
                    glm::vec3 transformed = node->worldTransform * glm::vec4(corner, 1.0f);
                    mesh->worldAABBMin = glm::min(mesh->worldAABBMin, transformed);
                    mesh->worldAABBMax = glm::max(mesh->worldAABBMax, transformed);
                }
            }
        }
        node->isTransformDirty = false;
    }
    for (const auto& child : node->children) {
        updateNode(child, node->worldTransform);
    }
}

void Scene::addMeshToNode(std::shared_ptr<Node> node, std::shared_ptr<Mesh> mesh) {
    if (!node->meshGroup) {
        node->meshGroup = std::make_shared<MeshGroup>();
        node->meshGroup->name = node->name;
    }
    mesh->vertexOffset = vertices.size();
    mesh->indexOffset = indices.size();
    mesh->vertexCount = mesh->vertices.size();
    mesh->indexCount = mesh->indices.size();
    vertices.insert(vertices.end(), mesh->vertices.begin(), mesh->vertices.end());
    indices.insert(indices.end(), mesh->indices.begin(), mesh->indices.end());
    // TODO: clean up
    // mesh->vertices.clear();
    // mesh->indices.clear();
    node->meshGroup->meshes.push_back(mesh);
    if (mesh->material) {
        materials.push_back(mesh->material);
        if (mesh->material->albedoMap) {
            images.push_back(mesh->material->albedoMap);
        }
        if (mesh->material->normalMap) {
            images.push_back(mesh->material->normalMap);
        }
        if (mesh->material->metallicRoughnessMap) {
            images.push_back(mesh->material->metallicRoughnessMap);
        }
        if (mesh->material->occlusionMap) {
            images.push_back(mesh->material->occlusionMap);
        }
        if (mesh->material->displacementMap) {
            images.push_back(mesh->material->displacementMap);
        }
    }
}

// void Scene::addLightToNode(std::shared_ptr<Node> node, std::shared_ptr<Light> light) {
//     if (!node->light) {
//         node->light = light;
//     }
// }

// Usage example
// auto scene = Scene();
// auto entity = scene.createNode("Cube", glm::identity<glm::mat4>());
// scene.addMeshToNode(entity, MeshBuilder::buildCube(1.0f));