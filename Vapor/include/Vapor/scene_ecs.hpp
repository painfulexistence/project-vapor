#pragma once

#include <entt/entt.hpp>
#include <string>
#include <vector>
#include <memory>

#include "ecs_components.hpp"
#include "ecs_systems.hpp"
#include "graphics.hpp"

namespace Vapor {

/**
 * ECS-based Scene - A modern Entity-Component-System scene manager
 *
 * This replaces the traditional scene graph approach with a data-oriented
 * ECS architecture using EnTT, making gameplay code easier to write.
 *
 * Key benefits:
 * - Data locality and cache-friendly iteration
 * - Flexible component composition
 * - Easy to add new gameplay features
 * - Better performance for large numbers of entities
 */
class ECSScene {
public:
    ECSScene(const std::string& name = "Scene");
    ~ECSScene() = default;

    // === Entity Management ===

    /**
     * Create a new entity with optional name
     * Returns the entity handle
     */
    entt::entity createEntity(const std::string& name = "");

    /**
     * Destroy an entity and all its components
     */
    void destroyEntity(entt::entity entity);

    /**
     * Find entity by name (returns entt::null if not found)
     */
    entt::entity findEntity(const std::string& name);

    /**
     * Check if entity is valid
     */
    bool isValid(entt::entity entity) const;

    // === Component Management (Convenience Methods) ===

    /**
     * Add Transform component to entity
     */
    Transform& addTransform(entt::entity entity,
                           const glm::vec3& position = glm::vec3(0.0f),
                           const glm::quat& rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f),
                           const glm::vec3& scale = glm::vec3(1.0f));

    /**
     * Add MeshRenderer component to entity
     */
    MeshRenderer& addMeshRenderer(entt::entity entity, std::shared_ptr<Mesh> mesh);

    /**
     * Add RigidBody component to entity
     */
    RigidBody& addRigidBody(entt::entity entity, BodyHandle handle, float mass = 1.0f);

    /**
     * Set parent-child relationship between entities
     */
    void setParent(entt::entity child, entt::entity parent);

    /**
     * Add child entity to parent
     */
    void addChild(entt::entity parent, entt::entity child);

    // === Direct Registry Access ===

    /**
     * Get the underlying EnTT registry for advanced usage
     * Use this when you need full ECS power
     */
    entt::registry& getRegistry() { return registry; }
    const entt::registry& getRegistry() const { return registry; }

    // === Systems Update ===

    /**
     * Update scene (runs all systems)
     */
    void update(float dt);

    /**
     * Update transforms only
     */
    void updateTransforms();

    // === Legacy Scene Data (for rendering compatibility) ===

    std::string name;
    std::vector<std::shared_ptr<Image>> images;
    std::vector<std::shared_ptr<Material>> materials;

    // GPU-driven rendering buffers
    std::vector<VertexData> vertices;
    std::vector<Uint32> indices;
    BufferHandle vertexBuffer;
    BufferHandle indexBuffer;
    bool isGeometryDirty = true;

    std::vector<DirectionalLight> directionalLights;
    std::vector<PointLight> pointLights;

    // === Rendering Helpers ===

    /**
     * Collect all mesh data for GPU upload
     * This syncs ECS mesh components to the legacy vertex/index buffers
     */
    void rebuildRenderData();

    /**
     * Print scene debug information
     */
    void print() const;

private:
    entt::registry registry;

    // Helper to add mesh data to scene buffers
    void addMeshToBuffers(std::shared_ptr<Mesh> mesh);
};

} // namespace Vapor
