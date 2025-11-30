#include "Vapor/world.hpp"
#include <algorithm>

namespace Vapor {

Node* World::find(const std::string& name) const {
    auto it = nodesByName.find(name);
    if (it != nodesByName.end() && !it->second.empty()) {
        return it->second.front();
    }
    return nullptr;
}

std::vector<Node*> World::findAll(const std::string& name) const {
    auto it = nodesByName.find(name);
    if (it != nodesByName.end()) {
        return it->second;
    }
    return {};
}

std::vector<Node*> World::findByScene(SceneID sceneId) const {
    std::vector<Node*> result;
    for (Node* node : nodes) {
        if (node->sceneId == sceneId) {
            result.push_back(node);
        }
    }
    return result;
}

void World::forEach(std::function<void(Node*)> callback) {
    for (Node* node : nodes) {
        callback(node);
    }
}

void World::forEach(std::function<void(Node*)> callback) const {
    for (Node* node : nodes) {
        callback(node);
    }
}

void World::registerNode(Node* node) {
    nodes.push_back(node);
    if (!node->name.empty()) {
        nodesByName[node->name].push_back(node);
    }
}

void World::registerNodeRecursive(Node* node, SceneID sceneId) {
    node->sceneId = sceneId;
    registerNode(node);
    for (auto& child : node->children) {
        registerNodeRecursive(child.get(), sceneId);
    }
}

void World::unregisterNode(Node* node) {
    // Remove from flat list
    nodes.erase(std::remove(nodes.begin(), nodes.end(), node), nodes.end());

    // Remove from name lookup
    if (!node->name.empty()) {
        auto it = nodesByName.find(node->name);
        if (it != nodesByName.end()) {
            auto& vec = it->second;
            vec.erase(std::remove(vec.begin(), vec.end(), node), vec.end());
            if (vec.empty()) {
                nodesByName.erase(it);
            }
        }
    }
}

void World::unregisterScene(SceneID sceneId) {
    // Collect nodes to remove
    std::vector<Node*> toRemove;
    for (Node* node : nodes) {
        if (node->sceneId == sceneId) {
            toRemove.push_back(node);
        }
    }

    // Remove them
    for (Node* node : toRemove) {
        unregisterNode(node);
    }
}

} // namespace Vapor
