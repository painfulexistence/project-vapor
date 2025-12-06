#pragma once
#include <vector>
#include <unordered_map>
#include <string>
#include <memory>
#include <functional>
#include <cstdint>

#include "scene.hpp"

namespace Vapor {

class World {
public:
    World() = default;
    ~World() = default;

    // Non-copyable
    World(const World&) = delete;
    World& operator=(const World&) = delete;

    // Node queries
    Node* find(const std::string& name) const;
    std::vector<Node*> findAll(const std::string& name) const;
    std::vector<Node*> findByScene(SceneID sceneId) const;

    // Iterate all nodes
    void forEach(std::function<void(Node*)> callback);
    void forEach(std::function<void(Node*)> callback) const;

    // Node registration (called by SceneManager internally)
    void registerNode(Node* node);
    void registerNodeRecursive(Node* node, SceneID sceneId);
    void unregisterNode(Node* node);
    void unregisterScene(SceneID sceneId);

    // Stats
    size_t nodeCount() const { return nodes.size(); }

private:
    std::vector<Node*> nodes;
    std::unordered_map<std::string, std::vector<Node*>> nodesByName;
};

} // namespace Vapor
