#pragma once
#include <SDL3/SDL_stdinc.h>
#include <glm/mat4x4.hpp>
#include <memory>
#include <string>
#include <vector>

#include "graphics.hpp"
#include "physics_3d.hpp"

class FluidVolume;
struct FluidVolumeSettings;

// The renderer-side world: the geometry pool (shared vertex/index arrays and
// their GPU buffers), the meshes staged into it with baked world transforms,
// and the per-frame light lists gathered from the ECS (see LightGatherSystem).
// Analogous to Unreal's FScene / Bevy's RenderWorld.
//
// This is NOT a scene graph. Entity hierarchy, transforms, and gameplay state
// live in the entt registry (TransformComponent + systems); this class only
// holds what the renderer consumes. The old OOP Node tree that used to live
// here was retired once the ECS took over those roles.
class Scene {
public:
    std::string name;
    std::vector<std::shared_ptr<Vapor::Image>> images;
    std::vector<std::shared_ptr<Vapor::Material>> materials;
    // Light lists are overwritten every frame from the ECS; they are runtime
    // state for the renderer, not authored scene content.
    std::vector<DirectionalLight> directionalLights;
    std::vector<PointLight> pointLights;
    std::vector<RectLight> rectLights;
    std::vector<std::shared_ptr<FluidVolume>> fluidVolumes;

    // GPU-driven rendering: one shared vertex/index pool for the whole world.
    std::vector<Vapor::VertexData> vertices;
    std::vector<Uint32> indices;
    BufferHandle vertexBuffer;
    BufferHandle indexBuffer;
    std::vector<std::shared_ptr<Vapor::Mesh>> stagedMeshes;
    std::vector<glm::mat4> stagedMeshTransforms;

    bool isGeometryDirty = true;

    Scene() = default;
    Scene(const std::string& sceneName) : name(sceneName){};
    ~Scene() = default;

    // Append a mesh to the geometry pool with a baked world transform.
    void addMesh(std::shared_ptr<Vapor::Mesh> mesh, const glm::mat4& transform = glm::identity<glm::mat4>());

    std::shared_ptr<FluidVolume> createFluidVolume(Physics3D* physics, const FluidVolumeSettings& settings);
    void addFluidVolume(std::shared_ptr<FluidVolume> fluidVolume);
};
