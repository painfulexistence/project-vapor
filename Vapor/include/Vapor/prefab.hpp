#pragma once
#include "graphics.hpp"// Image, Material
#include "mesh.hpp"     // Mesh
#include <entt/entity/fwd.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/vec3.hpp>
#include <memory>
#include <string>
#include <vector>

namespace Vapor {

class RenderScene;

// A Prefab is the format-neutral, CPU-side representation an imported asset
// (glTF or USD) decodes into, before it becomes anything the renderer or the
// ECS knows about. It is the engine's single import target: ImportGLTFPrefab
// and ImportUSDPrefab both produce one, and Instantiate() turns it into entt
// entities.
//
// It is deliberately NOT a RenderScene, and its node list is NOT the retired
// OOP scene graph: a Prefab is an import-time *template* (import once,
// Instantiate many, then discard) — the live scene is always the entt
// registry. RenderScene is the renderer's flat GPU geometry pool. The flow:
//
//   ImportPrefab(path)                       // file  -> Prefab (this type)
//   Instantiate(registry, scene, prefab,...) // Prefab -> entt entities
//                                            //   (Transform + MeshRenderer +
//                                            //    Name, parented) AND registers
//                                            //   each mesh's geometry into the
//                                            //   scene's world pool once.
//   Renderer::collectDrawables(registry,...) // entities -> per-instance MDI,
//                                            //   each frame (references the
//                                            //   already-registered mesh)
//
//   Prefab prefab = ImportPrefab("models/helmet.glb");
//   if (prefab.ok) Instantiate(registry, scene, prefab, entt::null, "Helmet");

// A punctual light authored on a node. Mirrors KHR_lights_punctual / the USD
// UsdLux subset; Instantiate resolves it into the engine light components.
struct PrefabLight {
    enum class Type { Point, Directional, Spot };

    Type type = Type::Point;
    glm::vec3 color{ 1.0f };
    float intensity = 1.0f;
    float range = 0.0f;          // 0 = no falloff limit (always so for Directional)
    float innerConeAngle = 0.0f; // Spot only (radians)
    float outerConeAngle = 0.0f; // Spot only (radians)
};

// One node of the imported hierarchy, stored FLAT: `parent` is an index into
// the owning Prefab's `nodes` array (-1 for a top-level node). This is the same
// hierarchy information a nested tree would carry, but it maps 1:1 onto entt
// entities + TransformComponent::parent instead of reintroducing a tree object
// — consistent with "the ECS owns the scene". The transform is LOCAL; mesh /
// light entries index the Prefab's flat vectors, so a shared mesh is stored
// once.
struct PrefabNode {
    std::string name;
    glm::vec3 position{ 0.0f };
    glm::quat rotation{ 1.0f, 0.0f, 0.0f, 0.0f };
    glm::vec3 scale{ 1.0f };
    int parent = -1;        // index into Prefab::nodes, -1 = top level
    std::vector<int> meshes;// indices into Prefab::meshes
    std::vector<int> lights;// indices into Prefab::lights
};

struct Prefab {
    std::vector<PrefabNode> nodes;                  // flat, parent-indexed
    std::vector<std::shared_ptr<Mesh>> meshes;      // one entry per primitive
    std::vector<std::shared_ptr<Material>> materials;// referenced by Mesh::material
    std::vector<std::shared_ptr<Image>> images;     // referenced by materials
    std::vector<PrefabLight> lights;
    bool ok = false;
};

// Unified entry point: dispatches on file extension.
//   .gltf / .glb         -> ImportGLTFPrefab
//   .usd / .usda / .usdc -> ImportUSDPrefab
// Returns a Prefab with ok == false (and logs) on any failure.
Prefab ImportPrefab(const std::string& path);

Prefab ImportGLTFPrefab(const std::string& path);
Prefab ImportUSDPrefab(const std::string& path);

// Turns a Prefab into live entt entities under an optional parent. Registers
// each referenced mesh's geometry into `scene`'s world pool (once per Mesh, so
// repeated Instantiate calls of the same Prefab share geometry and draw as GPU
// instances), then creates one entity per node carrying TransformComponent
// (local, parented), MeshRendererComponent, NameComponent, and light
// components. Returns the entity created for the prefab's first top-level node
// (or entt::null if the prefab is empty / not ok).
entt::entity Instantiate(
    entt::registry& registry,
    RenderScene& scene,
    const Prefab& prefab,
    entt::entity parent = entt::null,
    const std::string& name = ""
);

}// namespace Vapor
