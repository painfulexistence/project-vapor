#pragma once
#include <SDL3/SDL_stdinc.h>
#include <glm/mat4x4.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/quaternion.hpp>
#include <string>
#include <vector>
#include <memory>

#include "graphics.hpp"
#include "physics_3d.hpp"

class CharacterController;
struct CharacterControllerSettings;

struct MeshGroup {
    std::string name;
    std::vector<std::shared_ptr<Mesh>> meshes;
};

struct Node {
    std::string name;
    std::vector<std::shared_ptr<Node>> children;
    glm::mat4 localTransform = glm::identity<glm::mat4>();
    glm::mat4 worldTransform = glm::identity<glm::mat4>(); // calculated from localTransform and parent's worldTransform
    std::shared_ptr<MeshGroup> meshGroup = nullptr;
    BodyHandle body;
    TriggerHandle trigger;
    std::unique_ptr<CharacterController> characterController;
    bool isTransformDirty = true;

    // Virtual callbacks for physics events (can be overridden in subclasses)
    virtual void onTriggerEnter(Node* other) {}
    virtual void onTriggerExit(Node* other) {}
    virtual void onCollisionEnter(Node* other) {}
    virtual void onCollisionExit(Node* other) {}

    glm::vec3 getLocalPosition() const {
        return glm::vec3(localTransform[3]);
    }
    glm::quat getLocalRotation() const {
        glm::mat3 rotation = glm::mat3(
            glm::normalize(glm::vec3(localTransform[0])),
            glm::normalize(glm::vec3(localTransform[1])),
            glm::normalize(glm::vec3(localTransform[2]))
        );
        return glm::quat_cast(rotation);
    }
    glm::vec3 getLocalEulerAngles() const {
        glm::quat currRot = getLocalRotation();
        return glm::eulerAngles(currRot);
    }
    glm::vec3 getLocalScale() const {
        return glm::vec3(
            glm::length(glm::vec3(localTransform[0])),
            glm::length(glm::vec3(localTransform[1])),
            glm::length(glm::vec3(localTransform[2]))
        );
    }
    glm::vec3 getWorldPosition() const {
        return glm::vec3(worldTransform[3]);
    }
    glm::quat getWorldRotation() const {
        glm::mat3 rotation = glm::mat3(
            glm::normalize(glm::vec3(worldTransform[0])),
            glm::normalize(glm::vec3(worldTransform[1])),
            glm::normalize(glm::vec3(worldTransform[2]))
        );
        return glm::quat_cast(rotation);
    }
    glm::vec3 getWorldEulerAngles() const {
        glm::quat currRot = getWorldRotation();
        return glm::eulerAngles(currRot);
    }
    glm::vec3 getWorldScale() const {
        return glm::vec3(
            glm::length(glm::vec3(worldTransform[0])),
            glm::length(glm::vec3(worldTransform[1])),
            glm::length(glm::vec3(worldTransform[2]))
        );
    }

    void setLocalPosition(const glm::vec3& position) {
        glm::vec3 currScale = getLocalScale();
        glm::quat currRotation = getLocalRotation();

        localTransform = glm::translate(glm::mat4(1.0f), position) * glm::mat4_cast(currRotation) * glm::scale(glm::mat4(1.0f), currScale);
        isTransformDirty = true;
    }
    void setLocalRotation(const glm::quat& rotation) {
        glm::vec3 currPosition = getLocalPosition();
        glm::vec3 currScale = getLocalScale();

        localTransform = glm::translate(glm::mat4(1.0f), currPosition) * glm::mat4_cast(rotation) * glm::scale(glm::mat4(1.0f), currScale);
        isTransformDirty = true;
    }
    void setLocalEulerAngles(const glm::vec3& eulerAngles) {
        setLocalRotation(glm::quat(eulerAngles));
    }
    void setLocalScale(const glm::vec3& scale) {
        if (scale.x == 0.0f || scale.y == 0.0f || scale.z == 0.0f) {
            return;
        }
        glm::vec3 currPosition = getLocalPosition();
        glm::quat currRotation = getLocalRotation();

        localTransform = glm::translate(glm::mat4(1.0f), currPosition) * glm::mat4_cast(currRotation) * glm::scale(glm::mat4(1.0f), scale);
        isTransformDirty = true;
    }
    void rotateAroundLocalAxis(const glm::vec3& axis, float angle) {
        glm::quat currRot = getLocalRotation();
        glm::quat deltaRot = glm::angleAxis(angle, glm::rotate(currRot, glm::normalize(axis)));
        setLocalRotation(deltaRot * currRot);
    }
    void rotateAroundWorldAxis(const glm::vec3& axis, float angle) {
        glm::quat currRot = getWorldRotation();
        glm::quat deltaRot = glm::angleAxis(angle, glm::normalize(axis));
        setLocalRotation(deltaRot * currRot);
    }
    void translate(const glm::vec3& offset) {
        setLocalPosition(getLocalPosition() + offset);
    }
    void rotate(const glm::vec3& axis, float angle) {
        rotateAroundWorldAxis(axis, angle);
    }
    void scale(const glm::vec3& factor) {
        setLocalScale(getLocalScale() * factor);
    }
    void setLocalTransform(const glm::mat4& transform) {
        localTransform = transform;
        isTransformDirty = true;
    }

    void setPosition(const glm::vec3& position) {
        glm::mat4 invParent = localTransform * glm::inverse(worldTransform);
        glm::vec3 localPos = glm::vec3(invParent * glm::vec4(position, 1.0f));
        setLocalPosition(localPos);
    }

    std::shared_ptr<Node> createChild(const std::string& name, const glm::mat4& localTransform) {
        auto child = std::make_shared<Node>();
        child->name = name;
        child->localTransform = localTransform;
        child->isTransformDirty = true;
        children.push_back(child);
        return child;
    }
    void addChild(std::shared_ptr<Node> child) {
        child->isTransformDirty = true;
        children.push_back(child);
    }

    // Character controller management
    void attachCharacterController(Physics3D* physics, const CharacterControllerSettings& settings);
    CharacterController* getCharacterController() { return characterController.get(); }
};

class Scene {
public:
    std::string name;
    std::vector<std::shared_ptr<Image>> images;
    std::vector<std::shared_ptr<Material>> materials;
    std::vector<std::shared_ptr<Node>> nodes;
    std::vector<DirectionalLight> directionalLights;
    std::vector<PointLight> pointLights;

    // GPU-driven rendering
    std::vector<VertexData> vertices;
    std::vector<Uint32> indices;
    BufferHandle vertexBuffer;
    BufferHandle indexBuffer;

    bool isGeometryDirty = true;

    Scene() = default;
    Scene(const std::string& name) : name(name) {};
    ~Scene() = default;

    void print();
    void update(float dt);

    std::shared_ptr<Node> createNode(const std::string& name, const glm::mat4& transform = glm::identity<glm::mat4>());
    void addNode(std::shared_ptr<Node> node);
    std::shared_ptr<Node> findNode(const std::string& name);
    std::shared_ptr<Node> findNodeInHierarchy(const std::string& name, const std::shared_ptr<Node>& node);

    void addMeshToNode(std::shared_ptr<Node> node, std::shared_ptr<Mesh> mesh);
    // void AddLightToNode(std::shared_ptr<Node> node, std::shared_ptr<Light> light);

private:
    void printNode(const std::shared_ptr<Node>& node);
    void updateNode(const std::shared_ptr<Node>& node, const glm::mat4& parentTransform);
};