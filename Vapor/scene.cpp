#include "scene.hpp"

#include <SDL3/SDL_log.h>


void Scene::Print() {
    SDL_Log("Scene %s", name.c_str());
    SDL_Log("--------------------------------");
    for (const auto& node : nodes) {
        PrintNode(node);
    }
}

void Scene::PrintNode(const std::shared_ptr<Node>& node) {
    SDL_Log("Node %s", node->name.c_str());
    SDL_Log("--------------------------------");
    if (node->meshGroup) {
        SDL_Log("meshes: %d", static_cast<int>(node->meshGroup->meshes.size()));
        for (const auto& mesh : node->meshGroup->meshes) {
            SDL_Log("Vertex count: %zu", mesh->vertices.size());
            if (mesh->indices.size() > 0) {
                SDL_Log("Index count: %zu", mesh->indices.size());
                for (const Uint32& idx : mesh->indices) {
                    SDL_Log(
                        "(Vertex %u) Position: %f, %f, %f, UV: %f, %f, Normal: %f, %f, %f",
                        idx,
                        mesh->vertices[idx].position.x,
                        mesh->vertices[idx].position.y,
                        mesh->vertices[idx].position.z,
                        mesh->vertices[idx].uv.x,
                        mesh->vertices[idx].uv.y,
                        mesh->vertices[idx].normal.x,
                        mesh->vertices[idx].normal.y,
                        mesh->vertices[idx].normal.z
                    );
                }
            }
        }
    }
    SDL_Log("--------------------------------");
    for (const auto& child : node->children) {
        PrintNode(child);
    }
}

std::shared_ptr<Node> Scene::CreateNode(const std::string& name, const glm::mat4& transform) {
    auto node = std::make_shared<Node>();
    node->name = name;
    node->localTransform = transform;
    nodes.push_back(node);
    return node;
}

void Scene::AddNode(std::shared_ptr<Node> node) {
    nodes.push_back(node);
}

std::shared_ptr<Node> Scene::FindNode(const std::string& name) {
    for (const auto& node : nodes) {
        auto result = FindNodeInHierarchy(name, node);
        if (result) {
            return result;
        }
    }
    return nullptr;
}

std::shared_ptr<Node> Scene::FindNodeInHierarchy(const std::string& name, const std::shared_ptr<Node>& node) {
    if (node->name == name) {
        return node;
    }
    for (const auto& childNode : node->children) {
        auto result = FindNodeInHierarchy(name, childNode);
        if (result) {
            return result;
        }
    }
    return nullptr;
}

void Scene::Update(float dt) {
    const glm::mat4 rootTransform = glm::scale(glm::mat4(1.0f), glm::vec3(1.0f));
    for (const auto& node : nodes) {
        UpdateNode(node, rootTransform);
    }
}

void Scene::UpdateNode(const std::shared_ptr<Node>& node, const glm::mat4& parentTransform) {
    if (node->isTransformDirty) {
        node->worldTransform = parentTransform * node->localTransform;
        node->isTransformDirty = false;
    }
    for (const auto& child : node->children) {
        UpdateNode(child, node->worldTransform);
    }
}

void Scene::AddMeshToNode(std::shared_ptr<Node> node, std::shared_ptr<Mesh> mesh) {
    if (!node->meshGroup) {
        node->meshGroup = std::make_shared<MeshGroup>();
        node->meshGroup->name = node->name;
    }
    node->meshGroup->meshes.push_back(mesh);
}

// void Scene::AddLightToNode(std::shared_ptr<Node> node, std::shared_ptr<Light> light) {
//     if (!node->light) {
//         node->light = light;
//     }
// }

// Usage example
// auto scene = Scene();
// auto entity = scene.CreateNode("Cube", glm::identity<glm::mat4>());
// scene.AddMeshToNode(entity, MeshBuilder::buildCube(1.0f));