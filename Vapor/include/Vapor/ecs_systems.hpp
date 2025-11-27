#pragma once

#include <entt/entt.hpp>
#include "ecs_components.hpp"

namespace Vapor {

// Forward declarations
class Physics3D;

// Transform System - updates world transforms based on hierarchy
class TransformSystem {
public:
    static void update(entt::registry& registry);

private:
    static void updateHierarchy(entt::registry& registry, entt::entity entity, const glm::mat4& parentTransform);
};

// Physics System - synchronizes physics bodies with transforms
class PhysicsSystem {
public:
    PhysicsSystem(Physics3D* physics) : physics(physics) {}

    void update(entt::registry& registry, float dt);
    void syncTransformsToPhysics(entt::registry& registry);
    void syncPhysicsToTransforms(entt::registry& registry);

private:
    Physics3D* physics;
};

} // namespace Vapor
