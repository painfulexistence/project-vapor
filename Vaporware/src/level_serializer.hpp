#pragma once
// Level serializer — Step 1: save only
//
// Format (cereal JSONOutputArchive):
//   { "level": { "version": 1, "gltf": {...}, "entities": [...] } }
//
// Skips entities tagged with SceneGeometryTag (GLTF-spawned mesh entities).
// Supported components: Name, Transform, AutoRotate.
// Further components will be added in subsequent steps.

#include "Vapor/components.hpp"
#include "components.hpp"
#include <cereal/archives/json.hpp>
#include <cereal/types/string.hpp>
#include <cereal/types/vector.hpp>
#include <entt/entt.hpp>
#include <fmt/core.h>
#include <fstream>
#include <string>

namespace LevelSerializer {

// ============================================================================
// Intermediate POD structs (cereal-serializable)
// ============================================================================

struct Vec3Data {
    float x = 0, y = 0, z = 0;
    template <class A>
    void serialize(A& a) {
        a(cereal::make_nvp("x", x), cereal::make_nvp("y", y), cereal::make_nvp("z", z));
    }
};

struct QuatData {
    float x = 0, y = 0, z = 0, w = 1;
    template <class A>
    void serialize(A& a) {
        a(cereal::make_nvp("x", x), cereal::make_nvp("y", y),
          cereal::make_nvp("z", z), cereal::make_nvp("w", w));
    }
};

struct TransformData {
    Vec3Data position;
    QuatData rotation;
    Vec3Data scale{ 1, 1, 1 };
    template <class A>
    void serialize(A& a) {
        a(cereal::make_nvp("position", position),
          cereal::make_nvp("rotation", rotation),
          cereal::make_nvp("scale", scale));
    }
};

struct AutoRotateData {
    Vec3Data axis{ 0, 1, 0 };
    float speed = 1.0f;
    template <class A>
    void serialize(A& a) {
        a(cereal::make_nvp("axis", axis), cereal::make_nvp("speed", speed));
    }
};

// One entry per serializable entity.
// has_* booleans allow round-trip without std::optional (cereal 1.3.x).
struct EntityData {
    std::string name;

    bool hasTransform = false;
    TransformData transform{};

    bool hasAutoRotate = false;
    AutoRotateData autoRotate{};

    template <class A>
    void serialize(A& a) {
        a(cereal::make_nvp("name", name),
          cereal::make_nvp("hasTransform", hasTransform),
          cereal::make_nvp("transform", transform),
          cereal::make_nvp("hasAutoRotate", hasAutoRotate),
          cereal::make_nvp("autoRotate", autoRotate));
    }
};

struct GltfRef {
    std::string path;
    bool optimized = true;
    template <class A>
    void serialize(A& a) {
        a(cereal::make_nvp("path", path), cereal::make_nvp("optimized", optimized));
    }
};

struct SceneData {
    int version = 1;
    GltfRef gltf;
    std::vector<EntityData> entities;

    template <class A>
    void serialize(A& a) {
        a(cereal::make_nvp("version", version),
          cereal::make_nvp("gltf", gltf),
          cereal::make_nvp("entities", entities));
    }
};

// ============================================================================
// Result type (used by the inspector for feedback)
// ============================================================================
struct SaveResult {
    bool ok = false;
    std::string error;
    int entityCount = 0;
    int skippedCount = 0;
};

// ============================================================================
// save()
// ============================================================================
// gltfPath / gltfOptimized: the GLTF scene file used; written as a reference
//   so the loader knows which file to re-instantiate on load.
// outPath: destination file (e.g. "scene.json")
inline SaveResult save(
    entt::registry& registry,
    const std::string& gltfPath,
    bool gltfOptimized,
    const std::string& outPath
) {
    SceneData level;
    level.gltf.path = gltfPath;
    level.gltf.optimized = gltfOptimized;

    int skipped = 0;

    registry.each([&](entt::entity entity) {
        // Skip GLTF-spawned mesh entities
        if (registry.all_of<SceneGeometryTag>(entity)) {
            ++skipped;
            return;
        }

        EntityData ed;

        // Name
        if (auto* n = registry.try_get<Vapor::NameComponent>(entity))
            ed.name = n->name;
        else
            ed.name = fmt::format("Entity_{}", entt::to_integral(entity));

        // Transform
        if (auto* t = registry.try_get<Vapor::TransformComponent>(entity)) {
            ed.hasTransform = true;
            ed.transform.position = { t->position.x, t->position.y, t->position.z };
            ed.transform.rotation = { t->rotation.x, t->rotation.y, t->rotation.z, t->rotation.w };
            ed.transform.scale    = { t->scale.x, t->scale.y, t->scale.z };
        }

        // AutoRotate
        if (auto* ar = registry.try_get<AutoRotateComponent>(entity)) {
            ed.hasAutoRotate = true;
            ed.autoRotate.axis  = { ar->axis.x, ar->axis.y, ar->axis.z };
            ed.autoRotate.speed = ar->speed;
        }

        level.entities.push_back(std::move(ed));
    });

    std::ofstream file(outPath);
    if (!file.is_open())
        return { false, fmt::format("Cannot open '{}' for writing", outPath), 0, skipped };

    try {
        cereal::JSONOutputArchive archive(file);
        archive(cereal::make_nvp("version", level.version),
                cereal::make_nvp("gltf", level.gltf),
                cereal::make_nvp("entities", level.entities));
    } catch (const std::exception& e) {
        return { false, fmt::format("Serialization error: {}", e.what()), 0, skipped };
    }

    return { true, {}, static_cast<int>(level.entities.size()), skipped };
}

} // namespace LevelSerializer
