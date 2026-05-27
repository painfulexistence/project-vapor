#pragma once
// Scene serializer — human-authored JSON composition file.
//
// Format:
//   {
//     "version": 1,
//     "gltf": { "path": "...", "optimized": true },
//     "entities": [
//       {
//         "name": "cube1",
//         "components": {
//           "transform":  { "position": [x,y,z], "rotation": [x,y,z,w], "scale": [x,y,z] },
//           "autoRotate": { "axis": [x,y,z], "speed": 1.5 }
//         }
//       }
//     ]
//   }
//
// Component presence is implicit: a key in "components" means the component
// exists. No has_* flags needed.
//
// Entities tagged SceneGeometryTag are GLTF-spawned meshes and are excluded;
// the GLTF file is referenced once in the top-level "gltf" block instead.

#include "Vapor/components.hpp"
#include "components.hpp"
#include <entt/entt.hpp>
#include <fmt/core.h>
#include <fstream>
#include <nlohmann/json.hpp>
#include <string>

namespace SceneSerializer {

using json = nlohmann::json;

// ============================================================================
// Result
// ============================================================================
struct SaveResult {
    bool        ok           = false;
    std::string error;
    int         entityCount  = 0;
    int         skippedCount = 0;
};

// ============================================================================
// Helpers
// ============================================================================
inline json toJson(const glm::vec3& v) { return { v.x, v.y, v.z }; }
inline json toJson(const glm::quat& q) { return { q.x, q.y, q.z, q.w }; }

// ============================================================================
// save()
// ============================================================================
inline SaveResult save(
    entt::registry&    registry,
    const std::string& gltfPath,
    bool               gltfOptimized,
    const std::string& outPath
) {
    json root;
    root["version"]  = 1;
    root["gltf"]     = { { "path", gltfPath }, { "optimized", gltfOptimized } };
    root["entities"] = json::array();

    int skipped = 0;

    for (auto entity : registry.storage<entt::entity>()) {
        if (registry.all_of<SceneGeometryTag>(entity)) { ++skipped; continue; }

        json e;
        e["name"] = registry.try_get<Vapor::NameComponent>(entity)
            ? registry.get<Vapor::NameComponent>(entity).name
            : fmt::format("Entity_{}", entt::to_integral(entity));

        json components = json::object();

        if (auto* t = registry.try_get<Vapor::TransformComponent>(entity))
            components["transform"] = {
                { "position", toJson(t->position) },
                { "rotation", toJson(t->rotation) },
                { "scale",    toJson(t->scale)    }
            };

        if (auto* ar = registry.try_get<AutoRotateComponent>(entity))
            components["autoRotate"] = {
                { "axis",  toJson(ar->axis) },
                { "speed", ar->speed        }
            };

        e["components"] = components;
        root["entities"].push_back(std::move(e));
    }

    std::ofstream file(outPath);
    if (!file.is_open())
        return { false, fmt::format("Cannot open '{}' for writing", outPath), 0, skipped };

    file << root.dump(2);

    return { true, {}, static_cast<int>(root["entities"].size()), skipped };
}

} // namespace SceneSerializer
