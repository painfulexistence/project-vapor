#pragma once
#include <SDL3/SDL_stdinc.h>
#include <glm/mat4x4.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <string>
#include <vector>
#include <memory>

#include "graphics.hpp"

struct MeshGroup {
    std::string name;
    std::vector<std::shared_ptr<Mesh>> meshes;
};

struct Node {
    std::string name;
    std::vector<std::shared_ptr<Node>> children;
    glm::mat4 localTransform;
    glm::mat4 worldTransform; // calculated from localTransform and parent's worldTransform
    std::shared_ptr<MeshGroup> meshGroup = nullptr;
    bool isTransformDirty = true;

    void SetLocalTransform(const glm::mat4& transform) {
        localTransform = transform;
        isTransformDirty = true;
    }

    std::shared_ptr<Node> CreateChild(const std::string& name, const glm::mat4& localTransform) {
        auto child = std::make_shared<Node>();
        child->name = name;
        child->localTransform = localTransform;
        child->isTransformDirty = true;
        children.push_back(child);
        return child;
    }
    void AddChild(std::shared_ptr<Node> child) {
        child->isTransformDirty = true;
        children.push_back(child);
    }
};

class Scene {
public:
    std::string name;
    std::vector<std::shared_ptr<Image>> images;
    std::vector<std::shared_ptr<Material>> materials;
    std::vector<std::shared_ptr<Node>> nodes;

    Scene() = default;
    Scene(const std::string& name) : name(name) {};
    ~Scene() = default;

    void Print();
    void Update(float dt);

    std::shared_ptr<Node> CreateNode(const std::string& name, const glm::mat4& transform = glm::identity<glm::mat4>());
    void AddNode(std::shared_ptr<Node> node);
    std::shared_ptr<Node> FindNode(const std::string& name);
    std::shared_ptr<Node> FindNodeInHierarchy(const std::string& name, const std::shared_ptr<Node>& node);

    void AddMeshToNode(std::shared_ptr<Node> node, std::shared_ptr<Mesh> mesh);
    // void AddLightToNode(std::shared_ptr<Node> node, std::shared_ptr<Light> light);

private:
    void PrintNode(const std::shared_ptr<Node>& node);
    void UpdateNode(const std::shared_ptr<Node>& node, const glm::mat4& parentTransform);
};