#pragma once

#include <entt/entt.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <glm/gtc/quaternion.hpp>
#include <string>
#include <vector>
#include <memory>

#include "graphics.hpp"
#include "physics_3d.hpp"

namespace Vapor {

// Entity identification
struct Name {
    std::string value;
};

// Transform component - replaces Node transform functionality
struct Transform {
    glm::vec3 position = glm::vec3(0.0f);
    glm::quat rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f); // Identity quaternion
    glm::vec3 scale = glm::vec3(1.0f);

    glm::mat4 localMatrix = glm::identity<glm::mat4>();
    glm::mat4 worldMatrix = glm::identity<glm::mat4>();

    bool isDirty = true;

    // Helper methods
    glm::mat4 computeLocalMatrix() const;
    void setPosition(const glm::vec3& pos);
    void setRotation(const glm::quat& rot);
    void setRotation(const glm::vec3& eulerAngles);
    void setScale(const glm::vec3& scl);
    void translate(const glm::vec3& offset);
    void rotate(const glm::vec3& axis, float angle);
    void scaleBy(const glm::vec3& factor);
};

// Hierarchy component for parent-child relationships
struct Hierarchy {
    entt::entity parent = entt::null;
    std::vector<entt::entity> children;
};

// Mesh rendering component
struct MeshRenderer {
    std::string name;
    std::vector<std::shared_ptr<Mesh>> meshes;
};

// Physics rigid body component
struct RigidBody {
    BodyHandle handle;
    float mass = 1.0f;
    bool isKinematic = false;
};

// Light components
struct DirectionalLightComponent {
    DirectionalLight light;
};

struct PointLightComponent {
    PointLight light;
};

// Tag components for filtering
struct Active {};
struct Visible {};

} // namespace Vapor
