#pragma once
// Vapor::SceneSerializer — extensible JSON scene file writer.
//
// Format:
//   {
//     "version": 1,
//     "gltf": { "path": "...", "optimized": true },
//     "entities": [
//       { "name": "cube1", "components": { "transform": {...}, "autoRotate": {...} } }
//     ]
//   }
//
// Engine pre-registers its own components (transform, meshRenderer).
// Game code registers additional component writers:
//
//   Vapor::SceneSerializer serializer;
//   serializer.registerComponent("autoRotate",
//       [](json& out, entt::registry& reg, entt::entity e) {
//           if (auto* c = reg.try_get<AutoRotateComponent>(e))
//               out = { {"axis", Vapor::toJson(c->axis)}, {"speed", c->speed} };
//       });
//
// The caller decides WHICH entities to serialize — typically via
// SceneInspector::setEntityProvider() or by building a vector manually:
//
//   std::vector<entt::entity> toSave;
//   for (auto e : reg.storage<entt::entity>())
//       if (!reg.all_of<SceneGeometryTag>(e)) toSave.push_back(e);
//   serializer.save(reg, toSave, gltfPath, optimized, outPath);

#include "Vapor/components.hpp"
#include <entt/entt.hpp>
#include <fmt/core.h>
#include <fstream>
#include <functional>
#include <nlohmann/json.hpp>
#include <span>
#include <string>
#include <vector>

namespace Vapor {

using json = nlohmann::json;

// ============================================================================
// Helpers — public so game-side writers can reuse them
// ============================================================================
inline json toJson(const glm::vec3& v) { return { v.x, v.y, v.z }; }
inline json toJson(const glm::quat& q) { return { q.x, q.y, q.z, q.w }; }

// ============================================================================
// SceneSerializer
// ============================================================================
class SceneSerializer {
public:
    struct SaveResult {
        bool        ok          = false;
        std::string error;
        int         entityCount = 0;
    };

    // Write component data into `out`.
    // Leave `out` as json null if the entity doesn't have this component.
    using ComponentWriter = std::function<void(json& out, entt::registry&, entt::entity)>;

    SceneSerializer() { registerEngineComponents(); }

    // Register a game-specific component writer.
    // `key` becomes the key in the "components" JSON object.
    void registerComponent(std::string key, ComponentWriter writer) {
        m_writers.push_back({ std::move(key), std::move(writer) });
    }

    // Serialize only the entities in `entities`.
    // The caller is responsible for filtering (e.g. exclude SceneGeometryTag).
    [[nodiscard]] SaveResult save(
        entt::registry&               registry,
        std::span<const entt::entity> entities,
        const std::string&            gltfPath,
        bool                          gltfOptimized,
        const std::string&            outPath
    ) {
        json root;
        root["version"]  = 1;
        root["gltf"]     = { { "path", gltfPath }, { "optimized", gltfOptimized } };
        root["entities"] = json::array();

        for (entt::entity entity : entities) {
            if (!registry.valid(entity)) continue;

            json e;
            e["name"] = registry.try_get<NameComponent>(entity)
                ? registry.get<NameComponent>(entity).name
                : fmt::format("Entity_{}", entt::to_integral(entity));

            json components = json::object();
            for (auto& [key, writer] : m_writers) {
                json val;
                writer(val, registry, entity);
                if (!val.is_null()) components[key] = std::move(val);
            }

            e["components"] = components;
            root["entities"].push_back(std::move(e));
        }

        std::ofstream file(outPath);
        if (!file.is_open())
            return { false, fmt::format("Cannot open '{}' for writing", outPath), 0 };

        file << root.dump(2);
        return { true, {}, static_cast<int>(root["entities"].size()) };
    }

private:
    struct WriterEntry {
        std::string     key;
        ComponentWriter fn;
    };
    std::vector<WriterEntry> m_writers;

    void registerEngineComponents() {
        registerComponent("transform", [](json& out, entt::registry& reg, entt::entity e) {
            if (auto* t = reg.try_get<TransformComponent>(e))
                out = {
                    { "position", toJson(t->position) },
                    { "rotation", toJson(t->rotation) },
                    { "scale",    toJson(t->scale)    }
                };
        });
        // meshRenderer — only visibility; mesh data lives in the gltf block
        registerComponent("meshRenderer", [](json& out, entt::registry& reg, entt::entity e) {
            if (auto* m = reg.try_get<MeshRendererComponent>(e))
                out = { { "visible", m->visible } };
        });
    }
};

} // namespace Vapor
