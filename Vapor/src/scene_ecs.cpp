#include "scene_ecs.hpp"
#include <fmt/core.h>

namespace Vapor {

ECSScene::ECSScene(const std::string& sceneName)
    : name(sceneName) {
}

entt::entity ECSScene::createEntity(const std::string& entityName) {
    entt::entity entity = registry.create();

    if (!entityName.empty()) {
        auto& name = registry.emplace<Name>(entity);
        name.value = entityName;
    }

    // Add Active tag by default
    registry.emplace<Active>(entity);

    return entity;
}

void ECSScene::destroyEntity(entt::entity entity) {
    if (!registry.valid(entity)) {
        return;
    }

    // Remove from parent's children list
    if (auto* hierarchy = registry.try_get<Hierarchy>(entity)) {
        if (hierarchy->parent != entt::null && registry.valid(hierarchy->parent)) {
            auto& parentHierarchy = registry.get<Hierarchy>(hierarchy->parent);
            auto it = std::find(parentHierarchy.children.begin(),
                              parentHierarchy.children.end(),
                              entity);
            if (it != parentHierarchy.children.end()) {
                parentHierarchy.children.erase(it);
            }
        }

        // Recursively destroy children
        for (auto child : hierarchy->children) {
            destroyEntity(child);
        }
    }

    registry.destroy(entity);
}

entt::entity ECSScene::findEntity(const std::string& entityName) {
    auto view = registry.view<Name>();
    for (auto entity : view) {
        auto& name = view.get<Name>(entity);
        if (name.value == entityName) {
            return entity;
        }
    }
    return entt::null;
}

bool ECSScene::isValid(entt::entity entity) const {
    return registry.valid(entity);
}

Transform& ECSScene::addTransform(entt::entity entity,
                                 const glm::vec3& position,
                                 const glm::quat& rotation,
                                 const glm::vec3& scale) {
    auto& transform = registry.emplace<Transform>(entity);
    transform.position = position;
    transform.rotation = rotation;
    transform.scale = scale;
    transform.isDirty = true;
    return transform;
}

MeshRenderer& ECSScene::addMeshRenderer(entt::entity entity, std::shared_ptr<Mesh> mesh) {
    auto& renderer = registry.emplace<MeshRenderer>(entity);
    renderer.meshes.push_back(mesh);

    // Add mesh to scene buffers
    addMeshToBuffers(mesh);

    // Mark as visible
    if (!registry.all_of<Visible>(entity)) {
        registry.emplace<Visible>(entity);
    }

    return renderer;
}

RigidBody& ECSScene::addRigidBody(entt::entity entity, BodyHandle handle, float mass) {
    auto& body = registry.emplace<RigidBody>(entity);
    body.handle = handle;
    body.mass = mass;
    body.isKinematic = (mass == 0.0f);
    return body;
}

void ECSScene::setParent(entt::entity child, entt::entity parent) {
    if (!registry.valid(child) || !registry.valid(parent)) {
        return;
    }

    // Get or create hierarchy components
    auto& childHierarchy = registry.get_or_emplace<Hierarchy>(child);
    auto& parentHierarchy = registry.get_or_emplace<Hierarchy>(parent);

    // Remove from old parent if exists
    if (childHierarchy.parent != entt::null && registry.valid(childHierarchy.parent)) {
        auto& oldParentHierarchy = registry.get<Hierarchy>(childHierarchy.parent);
        auto it = std::find(oldParentHierarchy.children.begin(),
                          oldParentHierarchy.children.end(),
                          child);
        if (it != oldParentHierarchy.children.end()) {
            oldParentHierarchy.children.erase(it);
        }
    }

    // Set new parent
    childHierarchy.parent = parent;
    parentHierarchy.children.push_back(child);

    // Mark child transform as dirty
    if (auto* transform = registry.try_get<Transform>(child)) {
        transform->isDirty = true;
    }
}

void ECSScene::addChild(entt::entity parent, entt::entity child) {
    setParent(child, parent);
}

void ECSScene::update(float dt) {
    // Update transforms (hierarchy propagation)
    updateTransforms();

    // Physics system would go here
    // Rendering preparation would go here
}

void ECSScene::updateTransforms() {
    TransformSystem::update(registry);
}

void ECSScene::rebuildRenderData() {
    vertices.clear();
    indices.clear();

    auto view = registry.view<MeshRenderer, Visible>();

    for (auto entity : view) {
        auto& meshRenderer = view.get<MeshRenderer>(entity);

        for (auto& mesh : meshRenderer.meshes) {
            // Update offsets
            mesh->vertexOffset = vertices.size();
            mesh->indexOffset = indices.size();
            mesh->vertexCount = mesh->vertices.size();
            mesh->indexCount = mesh->indices.size();

            // Add to scene buffers
            vertices.insert(vertices.end(), mesh->vertices.begin(), mesh->vertices.end());
            indices.insert(indices.end(), mesh->indices.begin(), mesh->indices.end());

            // Add material and images
            if (mesh->material) {
                materials.push_back(mesh->material);

                if (mesh->material->albedoMap) images.push_back(mesh->material->albedoMap);
                if (mesh->material->normalMap) images.push_back(mesh->material->normalMap);
                if (mesh->material->metallicMap) images.push_back(mesh->material->metallicMap);
                if (mesh->material->roughnessMap) images.push_back(mesh->material->roughnessMap);
                if (mesh->material->occlusionMap) images.push_back(mesh->material->occlusionMap);
                if (mesh->material->displacementMap) images.push_back(mesh->material->displacementMap);
            }
        }
    }

    isGeometryDirty = true;
}

void ECSScene::print() const {
    fmt::print("ECS Scene: {}\n", name);
    fmt::print("  Entities: {}\n", registry.size());
    fmt::print("  Images: {}\n", images.size());
    fmt::print("  Materials: {}\n", materials.size());
    fmt::print("  Vertices: {}, Indices: {}\n", vertices.size(), indices.size());

    // Print component statistics
    auto transformView = registry.view<Transform>();
    auto meshView = registry.view<MeshRenderer>();
    auto bodyView = registry.view<RigidBody>();

    fmt::print("  Components:\n");
    fmt::print("    Transforms: {}\n", transformView.size_hint());
    fmt::print("    MeshRenderers: {}\n", meshView.size_hint());
    fmt::print("    RigidBodies: {}\n", bodyView.size_hint());

    fmt::print("  Lights:\n");
    fmt::print("    Directional: {}\n", directionalLights.size());
    fmt::print("    Point: {}\n", pointLights.size());
}

void ECSScene::addMeshToBuffers(std::shared_ptr<Mesh> mesh) {
    mesh->vertexOffset = vertices.size();
    mesh->indexOffset = indices.size();
    mesh->vertexCount = mesh->vertices.size();
    mesh->indexCount = mesh->indices.size();

    vertices.insert(vertices.end(), mesh->vertices.begin(), mesh->vertices.end());
    indices.insert(indices.end(), mesh->indices.begin(), mesh->indices.end());

    // Add material and images if not already present
    if (mesh->material) {
        // Simple approach: just add (duplicate check can be added later)
        materials.push_back(mesh->material);

        if (mesh->material->albedoMap) images.push_back(mesh->material->albedoMap);
        if (mesh->material->normalMap) images.push_back(mesh->material->normalMap);
        if (mesh->material->metallicMap) images.push_back(mesh->material->metallicMap);
        if (mesh->material->roughnessMap) images.push_back(mesh->material->roughnessMap);
        if (mesh->material->occlusionMap) images.push_back(mesh->material->occlusionMap);
        if (mesh->material->displacementMap) images.push_back(mesh->material->displacementMap);
    }

    isGeometryDirty = true;
}

} // namespace Vapor
