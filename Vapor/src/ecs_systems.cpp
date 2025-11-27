#include "ecs_systems.hpp"
#include "physics_3d.hpp"
#include <glm/gtc/matrix_transform.hpp>

namespace Vapor {

// TransformSystem implementation
void TransformSystem::update(entt::registry& registry) {
    // First, update all root entities (those without parents)
    auto view = registry.view<Transform>();

    for (auto entity : view) {
        auto* hierarchy = registry.try_get<Hierarchy>(entity);

        // If entity has no parent or parent is null, it's a root
        if (!hierarchy || hierarchy->parent == entt::null) {
            updateHierarchy(registry, entity, glm::identity<glm::mat4>());
        }
    }
}

void TransformSystem::updateHierarchy(entt::registry& registry, entt::entity entity, const glm::mat4& parentTransform) {
    auto& transform = registry.get<Transform>(entity);

    // Update local matrix if dirty
    if (transform.isDirty) {
        transform.localMatrix = transform.computeLocalMatrix();
        transform.isDirty = false;
    }

    // Compute world matrix
    transform.worldMatrix = parentTransform * transform.localMatrix;

    // Recursively update children
    if (auto* hierarchy = registry.try_get<Hierarchy>(entity)) {
        for (auto child : hierarchy->children) {
            if (registry.valid(child)) {
                updateHierarchy(registry, child, transform.worldMatrix);
            }
        }
    }
}

// PhysicsSystem implementation
void PhysicsSystem::update(entt::registry& registry, float dt) {
    if (!physics) return;

    // Sync transforms to physics before simulation
    syncTransformsToPhysics(registry);

    // Run physics simulation
    physics->process(dt);

    // Sync physics results back to transforms
    syncPhysicsToTransforms(registry);
}

void PhysicsSystem::syncTransformsToPhysics(entt::registry& registry) {
    if (!physics) return;

    auto view = registry.view<Transform, RigidBody>();

    for (auto entity : view) {
        auto& transform = view.get<Transform>(entity);
        auto& body = view.get<RigidBody>(entity);

        if (!body.isKinematic && body.handle.isValid()) {
            // For dynamic bodies, we read from physics
            // For kinematic bodies, we write to physics
            continue;
        }

        // Update kinematic body position/rotation from transform
        if (body.handle.isValid()) {
            glm::vec3 pos = transform.position;
            glm::quat rot = transform.rotation;
            physics->setBodyTransform(body.handle, pos, rot);
        }
    }
}

void PhysicsSystem::syncPhysicsToTransforms(entt::registry& registry) {
    if (!physics) return;

    auto view = registry.view<Transform, RigidBody>();

    for (auto entity : view) {
        auto& transform = view.get<Transform>(entity);
        auto& body = view.get<RigidBody>(entity);

        // Only sync dynamic bodies from physics to transform
        if (!body.isKinematic && body.handle.isValid()) {
            glm::vec3 pos;
            glm::quat rot;
            physics->getBodyTransform(body.handle, pos, rot);

            transform.position = pos;
            transform.rotation = rot;
            transform.isDirty = true;
        }
    }
}

} // namespace Vapor
