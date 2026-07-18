#include "scene_blueprint.hpp"

#include "asset_manager.hpp"
#include "components.hpp"
#include "file_system.hpp"
#include "render_scene.hpp"

#include <algorithm>
#include <cstdint>
#include <fmt/core.h>
#include <fstream>
#include <glm/gtc/quaternion.hpp>
#include <glm/matrix.hpp>
#include <nlohmann/json.hpp>
#include <sstream>
#include <unordered_set>

namespace Vapor {

using json = nlohmann::json;

// ── JSON field helpers ───────────────────────────────────────────────────────

static glm::vec3 readVec3(const json& j, const char* key, glm::vec3 fallback) {
    if (!j.contains(key)) return fallback;
    const auto& a = j.at(key);
    if (!a.is_array() || a.size() < 3) return fallback;
    return { a[0].get<float>(), a[1].get<float>(), a[2].get<float>() };
}

// Rotation: "rotation" is a quaternion [x, y, z, w] (glTF layout);
// "rotationEuler" is XYZ degrees for hand-authored scenes. Quaternion wins if
// both are present.
static glm::quat readRotation(const json& j) {
    if (j.contains("rotation")) {
        const auto& a = j.at("rotation");
        if (a.is_array() && a.size() >= 4)
            return glm::quat(a[3].get<float>(), a[0].get<float>(), a[1].get<float>(), a[2].get<float>());
    }
    if (j.contains("rotationEuler")) {
        const auto& a = j.at("rotationEuler");
        if (a.is_array() && a.size() >= 3) {
            const glm::vec3 rad = glm::radians(glm::vec3(a[0].get<float>(), a[1].get<float>(), a[2].get<float>()));
            return glm::quat(rad);
        }
    }
    return glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
}

static LightBlueprint parseLight(const json& j) {
    LightBlueprint light;
    const std::string type = j.value("type", "point");
    if (type == "directional")
        light.type = LightBlueprint::Type::Directional;
    else if (type == "spot")
        light.type = LightBlueprint::Type::Spot;
    else
        light.type = LightBlueprint::Type::Point;
    light.color = readVec3(j, "color", glm::vec3(1.0f));
    light.intensity = j.value("intensity", 1.0f);
    light.range = j.value("range", 0.0f);
    // Cone angles are authored in degrees in scene JSON (human-friendly), but
    // stored in radians (the glTF/USD convention the importers emit).
    if (j.contains("innerConeDeg")) light.innerConeAngle = glm::radians(j.value("innerConeDeg", 0.0f));
    if (j.contains("outerConeDeg")) light.outerConeAngle = glm::radians(j.value("outerConeDeg", 45.0f));
    return light;
}

// ── Parse (pure, no I/O) ─────────────────────────────────────────────────────

static void parseEntityRec(const json& j, int parentIndex, SceneBlueprint& out) {
    EntityBlueprint e;
    e.name = j.value("name", "");
    e.position = readVec3(j, "position", glm::vec3(0.0f));
    e.rotation = readRotation(j);
    e.scale = readVec3(j, "scale", glm::vec3(1.0f));
    e.parent = parentIndex;
    e.source = j.value("source", "");
    e.prefab = j.value("prefab", "");
    if (j.contains("light")) {
        out.lights.push_back(parseLight(j.at("light")));
        e.lights.push_back(static_cast<int>(out.lights.size()) - 1);
    }
    const int selfIndex = static_cast<int>(out.entities.size());
    out.entities.push_back(std::move(e));
    if (j.contains("children")) {
        for (const auto& c : j.at("children"))
            parseEntityRec(c, selfIndex, out);
    }
}

SceneBlueprint parseSceneBlueprint(const std::string& jsonText, const std::string& nameHint) {
    SceneBlueprint bp;
    json root = json::parse(jsonText, /*cb=*/nullptr, /*allow_exceptions=*/false);
    if (root.is_discarded() || !root.is_object()) {
        fmt::print(stderr, "parseSceneBlueprint: malformed JSON ({})\n", nameHint);
        return bp;
    }
    bp.name = root.value("name", nameHint);
    if (root.contains("entities")) {
        for (const auto& e : root.at("entities"))
            parseEntityRec(e, -1, bp);
    }
    bp.ok = true;
    return bp;
}

// ── Splice ───────────────────────────────────────────────────────────────────

void appendBlueprint(SceneBlueprint& dst, SceneBlueprint&& sub, int parentIndex) {
    const int entityBase = static_cast<int>(dst.entities.size());
    const int meshBase = static_cast<int>(dst.meshes.size());
    const int lightBase = static_cast<int>(dst.lights.size());

    for (auto& e : sub.entities) {
        e.parent = e.parent < 0 ? parentIndex : e.parent + entityBase;
        for (int& m : e.meshes)
            m += meshBase;
        for (int& l : e.lights)
            l += lightBase;
        dst.entities.push_back(std::move(e));
    }
    std::move(sub.meshes.begin(), sub.meshes.end(), std::back_inserter(dst.meshes));
    std::move(sub.materials.begin(), sub.materials.end(), std::back_inserter(dst.materials));
    std::move(sub.images.begin(), sub.images.end(), std::back_inserter(dst.images));
    std::move(sub.lights.begin(), sub.lights.end(), std::back_inserter(dst.lights));
    std::move(sub.sources.begin(), sub.sources.end(), std::back_inserter(dst.sources));
}

// ── Load + expand ────────────────────────────────────────────────────────────

SceneBlueprint loadSceneBlueprint(const std::string& path) {
    auto resolved = FileSystem::instance().resolvePath(path);
    if (!resolved) {
        fmt::print(stderr, "loadSceneBlueprint: '{}' not found in any search path\n", path);
        return {};
    }
    std::ifstream file(*resolved, std::ios::binary);
    std::stringstream buffer;
    buffer << file.rdbuf();
    const std::string text = buffer.str();

    SceneBlueprint bp = parseSceneBlueprint(text, path);
    if (!bp.ok) return bp;

    // Expand source/prefab references. Iterate by index over the original
    // entity count only: appendBlueprint grows the array at the end, and
    // freshly spliced sub-blueprints arrive already expanded.
    const size_t originalCount = bp.entities.size();
    for (size_t i = 0; i < originalCount; ++i) {
        // Copies, not references: appendBlueprint may reallocate bp.entities.
        const std::string source = bp.entities[i].source;
        const std::string prefab = bp.entities[i].prefab;
        if (!source.empty()) {
            SceneBlueprint model = AssetManager::loadModel(source);
            if (model.ok) {
                bp.sources.push_back(source);
                appendBlueprint(bp, std::move(model), static_cast<int>(i));
            } else {
                fmt::print(stderr, "loadSceneBlueprint: '{}' failed to import source '{}'\n", path, source);
            }
        }
        if (!prefab.empty()) {
            SceneBlueprint nested = loadSceneBlueprint(prefab);
            if (nested.ok) {
                bp.sources.push_back(prefab);
                appendBlueprint(bp, std::move(nested), static_cast<int>(i));
            } else {
                fmt::print(stderr, "loadSceneBlueprint: '{}' failed to load prefab '{}'\n", path, prefab);
            }
        }
    }
    return bp;
}

// ── Transform decompose ──────────────────────────────────────────────────────

void decomposeTransform(const glm::mat4& m, glm::vec3& position, glm::quat& rotation, glm::vec3& scale) {
    position = glm::vec3(m[3]);
    scale = glm::vec3(glm::length(glm::vec3(m[0])), glm::length(glm::vec3(m[1])), glm::length(glm::vec3(m[2])));
    // A negative determinant means one axis is mirrored; fold the flip into X
    // so the rotation part stays proper (quat_cast requires det == +1).
    if (glm::determinant(glm::mat3(m)) < 0.0f) scale.x = -scale.x;
    glm::mat3 rot(1.0f);
    if (scale.x != 0.0f) rot[0] = glm::vec3(m[0]) / scale.x;
    if (scale.y != 0.0f) rot[1] = glm::vec3(m[1]) / scale.y;
    if (scale.z != 0.0f) rot[2] = glm::vec3(m[2]) / scale.z;
    rotation = glm::quat_cast(rot);
}

// ── Instantiate ──────────────────────────────────────────────────────────────

entt::entity instantiate(
    entt::registry& registry,
    RenderScene& scene,
    const SceneBlueprint& blueprint,
    entt::entity parent,
    const std::string& name,
    std::vector<entt::entity>* outEntities
) {
    if (!blueprint.ok) return entt::null;

    // Stage every referenced mesh into the scene's world pool exactly once.
    // Renderer::stage() registers geometry from mesh->vertices and the material
    // straight off mesh->material, so staging is just membership here; the
    // baked-transform slot is unused on the ECS path (drawable transforms come
    // from the entities).
    std::unordered_set<const Mesh*> staged;
    staged.reserve(scene.stagedMeshes.size());
    for (const auto& m : scene.stagedMeshes)
        staged.insert(m.get());
    for (const auto& e : blueprint.entities) {
        for (int mi : e.meshes) {
            const auto& mesh = blueprint.meshes[static_cast<size_t>(mi)];
            if (!mesh || mesh->renderMeshId != UINT32_MAX || staged.count(mesh.get())) continue;
            scene.stagedMeshes.push_back(mesh);
            scene.stagedMeshTransforms.push_back(glm::mat4(1.0f));
            staged.insert(mesh.get());
        }
    }
    // Mirror the payload into the scene lists so the ImGui Scene Materials /
    // textures editors see it (rendering itself doesn't read these).
    {
        std::unordered_set<const Material*> knownMats;
        for (const auto& m : scene.materials)
            knownMats.insert(m.get());
        for (const auto& m : blueprint.materials)
            if (m && !knownMats.count(m.get())) scene.materials.push_back(m);
        std::unordered_set<const Image*> knownImages;
        for (const auto& img : scene.images)
            knownImages.insert(img.get());
        for (const auto& img : blueprint.images)
            if (img && !knownImages.count(img.get())) scene.images.push_back(img);
    }

    // Root entity: the blueprint's mount point under `parent`.
    const entt::entity root = registry.create();
    registry.emplace<NameComponent>(root, NameComponent{ name.empty() ? blueprint.name : name });
    auto& rootTc = registry.emplace<TransformComponent>(root);
    rootTc.parent = parent;
    rootTc.isDirty = true;
    if (outEntities) outEntities->push_back(root);

    // Entities. Parents precede children in the array, so the parent's entt
    // entity (and accumulated world rotation, needed for directional lights)
    // is always available by the time a child is created.
    std::vector<entt::entity> created(blueprint.entities.size());
    std::vector<glm::quat> worldRot(blueprint.entities.size());
    for (size_t i = 0; i < blueprint.entities.size(); ++i) {
        const EntityBlueprint& e = blueprint.entities[i];
        const entt::entity ent = registry.create();
        created[i] = ent;
        const glm::quat parentRot = e.parent < 0 ? glm::quat(1.0f, 0.0f, 0.0f, 0.0f)
                                                 : worldRot[static_cast<size_t>(e.parent)];
        worldRot[i] = parentRot * e.rotation;

        registry.emplace<NameComponent>(
            ent, NameComponent{ e.name.empty() ? fmt::format("{}_{}", blueprint.name, i) : e.name }
        );
        auto& tc = registry.emplace<TransformComponent>(ent);
        tc.position = e.position;
        tc.rotation = e.rotation;
        tc.scale = e.scale;
        tc.parent = e.parent < 0 ? root : created[static_cast<size_t>(e.parent)];
        tc.isDirty = true;

        if (!e.meshes.empty()) {
            auto& mrc = registry.emplace<MeshRendererComponent>(ent);
            mrc.meshes.reserve(e.meshes.size());
            for (int mi : e.meshes)
                mrc.meshes.push_back(blueprint.meshes[static_cast<size_t>(mi)]);
        }

        for (int li : e.lights) {
            const LightBlueprint& light = blueprint.lights[static_cast<size_t>(li)];
            switch (light.type) {
            case LightBlueprint::Type::Point:
                registry.emplace<PointLightComponent>(
                    ent,
                    PointLightComponent{
                        .color = light.color,
                        .intensity = light.intensity,
                        // 0 means "unbounded" in glTF; the renderer wants a
                        // finite falloff, so give unbounded lights a generous one.
                        .radius = light.range > 0.0f ? light.range : 10.0f,
                    }
                );
                break;
            case LightBlueprint::Type::Directional:
                // KHR_lights_punctual: a directional light shines along the
                // node's world -Z.
                registry.emplace<DirectionalLightComponent>(
                    ent,
                    DirectionalLightComponent{
                        .direction = glm::normalize(worldRot[i] * glm::vec3(0.0f, 0.0f, -1.0f)),
                        .color = light.color,
                        .intensity = light.intensity,
                    }
                );
                break;
            case LightBlueprint::Type::Spot:
                // SpotLightComponent aims along the entity transform's forward
                // (rotation * -Z) — already carried by the TransformComponent.
                registry.emplace<SpotLightComponent>(
                    ent,
                    SpotLightComponent{
                        .color = light.color,
                        .intensity = light.intensity,
                        .radius = light.range > 0.0f ? light.range : 12.0f,
                        .innerAngle = glm::degrees(light.innerConeAngle),
                        .outerAngle = glm::degrees(light.outerConeAngle),
                    }
                );
                break;
            }
        }

        if (outEntities) outEntities->push_back(ent);
    }

    return root;
}

}// namespace Vapor
