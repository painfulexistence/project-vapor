#include "scene.hpp"

#include <SDL3/SDL_log.h>
#include <functional>

void Scene::print() {
    SDL_Log("Scene %s", name.c_str());
    SDL_Log("Total vertices: %d, total indices: %d", vertices.size(), indices.size());
    SDL_Log("--------------------------------");
    for (const auto& node : nodes) {
        printNode(node);
    }
}

void Scene::printNode(const std::shared_ptr<Node>& node) {
    SDL_Log("Node %s", node->name.c_str());
    SDL_Log("--------------------------------");
    if (node->meshGroup) {
        SDL_Log("meshes: %d", static_cast<int>(node->meshGroup->meshes.size()));
        for (const auto& mesh : node->meshGroup->meshes) {
            SDL_Log("Mesh: vertexOffset=%d, indexOffset=%d, vertexCount=%d, indexCount=%d",
                mesh->vertexOffset, mesh->indexOffset, mesh->vertexCount, mesh->indexCount);
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
    SDL_Log("--------------------------------");
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