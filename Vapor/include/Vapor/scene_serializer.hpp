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
// Engine pre-registers its own components (transform, meshRenderer…).
// Game code registers additional components and skip predicates:
//
//   Vapor::SceneSerializer serializer;
//   serializer.registerComponent("autoRotate",
//       [](json& out, entt::registry& reg, entt::entity e) {
//           if (auto* c = reg.try_get<AutoRotateComponent>(e))
//               out = { {"axis", Vapor::toJson(c->axis)}, {"speed", c->speed} };
//       });
//   serializer.registerSkipPredicate(
//       [](entt::registry& reg, entt::entity e) {
//           return reg.all_of<SceneGeometryTag>(e);
//       });

#include "Vapor/components.hpp"
#include <entt/entt.hpp>
#include <fmt/core.h>
#include <fstream>
#include <functional>
#include <nlohmann/json.hpp>
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
        bool        ok           = false;
        std::string error;
        int         entityCount  = 0;
        int         skippedCount = 0;
    };

    // ComponentWriter: write component data into `out`.
    // Leave `out` as json null if the entity doesn't have this component.
    using ComponentWriter  = std::function<void(json& out, entt::registry&, entt::entity)>;
    using SkipPredicate    = std::function<bool(entt::registry&, entt::entity)>;

    // Pre-registers engine components (transform, meshRenderer).
    SceneSerializer() { registerEngineComponents(); }

    // Register a game-specific component writer.
    // `key` becomes the key in the "components" JSON object.
    void registerComponent(std::string key, ComponentWriter writer) {
        m_writers.push_back({ std::move(key), std::move(writer) });
    }

    // Register a predicate; entities for which ANY predicate returns true are skipped.
    void registerSkipPredicate(SkipPredicate pred) {
        m_skipPredicates.push_back(std::move(pred));
    }

    SaveResult save(
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
            bool skip = false;
            for (auto& pred : m_skipPredicates) {
                if (pred(registry, entity)) { ++skipped; skip = true; break; }
            }
            if (skip) continue;

            json e;
            e["name"] = registry.try_get<NameComponent>(entity)
                ? registry.get<NameComponent>(entity).name
                : fmt::format("Entity_{}", entt::to_integral(entity));

            json components = json::object();
            for (auto& [key, writer] : m_writers) {
                json val;                               // null by default
                writer(val, registry, entity);
                if (!val.is_null()) components[key] = std::move(val);
            }

            e["components"] = components;
            root["entities"].push_back(std::move(e));
        }

        std::ofstream file(outPath);
        if (!file.is_open())
            return { false, fmt::format("Cannot open '{}' for writing", outPath), 0, skipped };

        file << root.dump(2);
        return { true, {}, static_cast<int>(root["entities"].size()), skipped };
    }

private:
    struct WriterEntry {
        std::string     key;
        ComponentWriter fn;
    };

    std::vector<WriterEntry>    m_writers;
    std::vector<SkipPredicate>  m_skipPredicates;

    void registerEngineComponents() {
        // transform
        registerComponent("transform", [](json& out, entt::registry& reg, entt::entity e) {
            if (auto* t = reg.try_get<TransformComponent>(e))
                out = {
                    { "position", toJson(t->position) },
                    { "rotation", toJson(t->rotation) },
                    { "scale",    toJson(t->scale)    }
                };
        });
        // meshRenderer — records visibility; mesh data is in the gltf block, not here
        registerComponent("meshRenderer", [](json& out, entt::registry& reg, entt::entity e) {
            if (auto* m = reg.try_get<MeshRendererComponent>(e))
                out = { { "visible", m->visible } };
        });
    }
};

} // namespace Vapor
