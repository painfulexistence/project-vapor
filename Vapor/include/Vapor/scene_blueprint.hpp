#pragma once
#include "graphics.hpp"// Image, Material, Mesh
#include <entt/entt.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/vec3.hpp>
#include <memory>
#include <string>
#include <vector>

namespace Vapor {

class RenderScene;

// ============================================================================
// Scene blueprints — the declarative authoring layer.
//
// A SceneBlueprint is the parse/import artifact every authored source decodes
// into before anything becomes live: a scene JSON parses into one, and a model
// file (glTF/USD) imports into one (its node hierarchy = entities, its
// meshes/materials/images = the payload). instantiate() turns a blueprint into
// entt entities; the blueprint is a TEMPLATE (parse once, instantiate many,
// then discard) — the live scene is always the registry, never the blueprint.
//
// scene JSON schema (see parseSceneBlueprint):
//   {
//     "name": "chapter1",
//     "entities": [
//       { "name": "Sun", "light": { "type": "directional", "intensity": 2 } },
//       { "name": "Helmet", "source": "models/helmet.glb",   // model ref
//         "position": [0, 1, 0], "children": [ ... ] },
//       { "name": "Door", "prefab": "prefabs/door.json" }    // nested blueprint
//     ]
//   }
//
// The flow:
//   loadSceneBlueprint("scenes/main.json")   // parse + expand source/prefab refs
//   instantiate(registry, scene, blueprint)  // blueprint -> entities; stages each
//                                            //   mesh into the RenderScene world
//                                            //   pool once (shared geometry ->
//                                            //   repeated instantiation draws as
//                                            //   GPU instances)
//   Renderer::collectDrawables(registry,...) // entities -> per-instance MDI, each
//                                            //   frame
// ============================================================================

// A punctual light authored on an entity. Mirrors KHR_lights_punctual / the
// UsdLux subset; instantiate() resolves it into the engine light components
// (position/orientation come from the entity's TransformComponent).
struct LightBlueprint {
    enum class Type { Point, Directional, Spot };

    Type type = Type::Point;
    glm::vec3 color{ 1.0f };
    float intensity = 1.0f;
    float range = 0.0f;                            // 0 = unbounded (importer default)
    float innerConeAngle = 0.0f;                   // Spot only, radians
    float outerConeAngle = glm::radians(45.0f);    // Spot only, radians
};

// One entity of the authored hierarchy, stored FLAT: `parent` is an index into
// the owning SceneBlueprint::entities (-1 = top level), and parents always
// precede their children in the array. This carries the same hierarchy a
// nested tree would, but maps 1:1 onto entt entities + TransformComponent::
// parent instead of reintroducing a scene-graph object — the ECS owns the
// scene. The transform is LOCAL; mesh/light entries index the blueprint's flat
// payload vectors, so a mesh shared by several entities is stored once.
struct EntityBlueprint {
    std::string name;
    glm::vec3 position{ 0.0f };
    glm::quat rotation{ 1.0f, 0.0f, 0.0f, 0.0f };
    glm::vec3 scale{ 1.0f };
    int parent = -1;        // index into SceneBlueprint::entities, -1 = top level
    std::vector<int> meshes;// indices into SceneBlueprint::meshes
    std::vector<int> lights;// indices into SceneBlueprint::lights

    // Authoring references, expanded by loadSceneBlueprint (kept afterwards as
    // provenance): a model file whose hierarchy is spliced under this entity,
    // or a nested scene JSON instantiated under it.
    std::string source;
    std::string prefab;
};

struct SceneBlueprint {
    std::string name;
    std::vector<EntityBlueprint> entities;// flat, parent-indexed; parents precede children

    // Payload decoded by the model importers, referenced by index from entities.
    std::vector<std::shared_ptr<Mesh>> meshes;
    std::vector<std::shared_ptr<Material>> materials;
    std::vector<std::shared_ptr<Image>> images;
    std::vector<LightBlueprint> lights;

    // Every file this blueprint was expanded from (the scene JSON itself is not
    // listed; source models and nested prefab JSONs are). Input set for the
    // cook-freshness hash.
    std::vector<std::string> sources;

    bool ok = false;
};

// Splice `sub` into `dst` under dst entity index `parentIndex` (-1 = top
// level): entities are appended with mesh/light/parent indices rebased, and
// the payload vectors are concatenated. Consumes `sub`.
void appendBlueprint(SceneBlueprint& dst, SceneBlueprint&& sub, int parentIndex);

// Parse scene JSON text into a blueprint. Pure and I/O-free: "source" /
// "prefab" references are recorded on the entities but NOT expanded — that is
// loadSceneBlueprint's job. Nested "children" arrays flatten into the
// parent-indexed layout. Returns ok == false on malformed JSON.
SceneBlueprint parseSceneBlueprint(const std::string& jsonText, const std::string& nameHint = "");

// Load a scene JSON through the FileSystem search paths, parse it, and expand
// every "source" (via AssetManager::loadModel) and "prefab" (recursively)
// reference. Returns ok == false (with a log) if the file is missing or
// malformed; a failed sub-reference logs and leaves that entity empty rather
// than failing the whole scene.
SceneBlueprint loadSceneBlueprint(const std::string& path);

// Decompose an affine transform into TRS (shear is folded into scale lossily —
// fine for DCC-authored content). Shared by the model importers.
void decomposeTransform(const glm::mat4& m, glm::vec3& position, glm::quat& rotation, glm::vec3& scale);

// Turn a blueprint into live entt entities. Creates one root entity (named
// `name`, or the blueprint's name) under `parent`, then one entity per
// EntityBlueprint carrying NameComponent, TransformComponent (local TRS,
// parented) and, where referenced, MeshRendererComponent and the engine light
// components. Every referenced mesh is staged into `scene` exactly once
// (skipped if already staged/registered), so instantiating the same blueprint
// repeatedly shares geometry and draws as GPU instances. Returns the root
// entity (entt::null if the blueprint is not ok); appends every created entity
// (root included) to *outEntities when given.
entt::entity instantiate(
    entt::registry& registry,
    RenderScene& scene,
    const SceneBlueprint& blueprint,
    entt::entity parent = entt::null,
    const std::string& name = "",
    std::vector<entt::entity>* outEntities = nullptr
);

}// namespace Vapor
