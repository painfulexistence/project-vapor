#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include "Vapor/components.hpp"
#include "Vapor/render_scene.hpp"
#include "Vapor/scene_blueprint.hpp"

using Catch::Approx;
using namespace Vapor;

// ── parseSceneBlueprint ─────────────────────────────────────────────────────

TEST_CASE("parse flattens nested children into parent indices", "[scene_blueprint]") {
    const char* json = R"({
        "name": "test",
        "entities": [
            { "name": "A", "position": [1, 2, 3],
              "children": [
                { "name": "B", "scale": [2, 2, 2],
                  "children": [ { "name": "C" } ] },
                { "name": "D" }
              ] },
            { "name": "E" }
        ]
    })";
    SceneBlueprint bp = parseSceneBlueprint(json);
    REQUIRE(bp.ok);
    REQUIRE(bp.name == "test");
    REQUIRE(bp.entities.size() == 5);

    // DFS order: A, B, C, D, E — parents always precede children.
    CHECK(bp.entities[0].name == "A");
    CHECK(bp.entities[0].parent == -1);
    CHECK(bp.entities[1].name == "B");
    CHECK(bp.entities[1].parent == 0);
    CHECK(bp.entities[2].name == "C");
    CHECK(bp.entities[2].parent == 1);
    CHECK(bp.entities[3].name == "D");
    CHECK(bp.entities[3].parent == 0);
    CHECK(bp.entities[4].name == "E");
    CHECK(bp.entities[4].parent == -1);

    CHECK(bp.entities[0].position.x == Approx(1.0f));
    CHECK(bp.entities[0].position.z == Approx(3.0f));
    CHECK(bp.entities[1].scale.x == Approx(2.0f));
}

TEST_CASE("parse reads lights, sources and rotations", "[scene_blueprint]") {
    const char* json = R"({
        "entities": [
            { "name": "Sun",
              "rotationEuler": [-45, 30, 0],
              "light": { "type": "directional", "color": [1, 0.9, 0.8], "intensity": 3 } },
            { "name": "Lamp",
              "light": { "type": "spot", "range": 8, "innerConeDeg": 15, "outerConeDeg": 35 } },
            { "name": "Helmet", "source": "models/helmet.glb" },
            { "name": "Door", "prefab": "prefabs/door.json" }
        ]
    })";
    SceneBlueprint bp = parseSceneBlueprint(json, "hint");
    REQUIRE(bp.ok);
    REQUIRE(bp.lights.size() == 2);

    CHECK(bp.lights[0].type == LightBlueprint::Type::Directional);
    CHECK(bp.lights[0].intensity == Approx(3.0f));
    CHECK(bp.lights[0].color.g == Approx(0.9f));
    REQUIRE(bp.entities[0].lights.size() == 1);
    CHECK(bp.entities[0].lights[0] == 0);
    // rotationEuler produced a non-identity quaternion
    CHECK(std::abs(bp.entities[0].rotation.w) < 0.999f);

    CHECK(bp.lights[1].type == LightBlueprint::Type::Spot);
    CHECK(bp.lights[1].range == Approx(8.0f));
    CHECK(bp.lights[1].innerConeAngle == Approx(glm::radians(15.0f)));
    CHECK(bp.lights[1].outerConeAngle == Approx(glm::radians(35.0f)));

    CHECK(bp.entities[2].source == "models/helmet.glb");
    CHECK(bp.entities[3].prefab == "prefabs/door.json");
}

TEST_CASE("parse rejects malformed JSON", "[scene_blueprint]") {
    CHECK_FALSE(parseSceneBlueprint("{ not json").ok);
    CHECK_FALSE(parseSceneBlueprint("[1, 2, 3]").ok);
}

// ── appendBlueprint ─────────────────────────────────────────────────────────

static std::shared_ptr<Mesh> makeMesh() {
    auto mesh = std::make_shared<Mesh>();
    mesh->vertices.resize(3);
    mesh->indices = { 0, 1, 2 };
    mesh->vertexCount = 3;
    mesh->indexCount = 3;
    return mesh;
}

TEST_CASE("appendBlueprint rebases entity, mesh and light indices", "[scene_blueprint]") {
    SceneBlueprint dst = parseSceneBlueprint(R"({ "entities": [ { "name": "Mount" } ] })");
    dst.meshes.push_back(makeMesh());
    dst.lights.push_back(LightBlueprint{});

    SceneBlueprint sub;
    sub.ok = true;
    sub.meshes.push_back(makeMesh());
    sub.lights.push_back(LightBlueprint{ .type = LightBlueprint::Type::Spot });
    EntityBlueprint root;
    root.name = "SubRoot";
    root.meshes = { 0 };
    root.lights = { 0 };
    sub.entities.push_back(root);
    EntityBlueprint child;
    child.name = "SubChild";
    child.parent = 0;
    sub.entities.push_back(child);
    sub.sources.push_back("sub.gltf");

    appendBlueprint(dst, std::move(sub), 0);

    REQUIRE(dst.entities.size() == 3);
    CHECK(dst.entities[1].name == "SubRoot");
    CHECK(dst.entities[1].parent == 0);          // sub top-level -> mount point
    CHECK(dst.entities[1].meshes[0] == 1);       // rebased past dst's existing mesh
    CHECK(dst.entities[1].lights[0] == 1);       // rebased past dst's existing light
    CHECK(dst.entities[2].name == "SubChild");
    CHECK(dst.entities[2].parent == 1);          // internal parent rebased
    REQUIRE(dst.meshes.size() == 2);
    REQUIRE(dst.lights.size() == 2);
    CHECK(dst.lights[1].type == LightBlueprint::Type::Spot);
    REQUIRE(dst.sources.size() == 1);
}

// ── decomposeTransform ──────────────────────────────────────────────────────

TEST_CASE("decomposeTransform round-trips TRS", "[scene_blueprint]") {
    const glm::vec3 pos(1.0f, -2.0f, 3.0f);
    const glm::quat rot = glm::angleAxis(glm::radians(40.0f), glm::normalize(glm::vec3(0.3f, 1.0f, 0.2f)));
    const glm::vec3 scl(2.0f, 0.5f, 3.0f);
    glm::mat4 m = glm::translate(glm::mat4(1.0f), pos) * glm::mat4_cast(rot) * glm::scale(glm::mat4(1.0f), scl);

    glm::vec3 outPos, outScl;
    glm::quat outRot;
    decomposeTransform(m, outPos, outRot, outScl);

    CHECK(outPos.x == Approx(pos.x));
    CHECK(outPos.y == Approx(pos.y));
    CHECK(outPos.z == Approx(pos.z));
    CHECK(outScl.x == Approx(scl.x));
    CHECK(outScl.y == Approx(scl.y));
    CHECK(outScl.z == Approx(scl.z));
    // Quaternion sign is ambiguous; compare via the dot product.
    CHECK(std::abs(glm::dot(outRot, rot)) == Approx(1.0f).margin(1e-4));
}

// ── instantiate ─────────────────────────────────────────────────────────────

TEST_CASE("instantiate builds parented entities and stages meshes once", "[scene_blueprint]") {
    SceneBlueprint bp = parseSceneBlueprint(R"({
        "name": "inst",
        "entities": [
            { "name": "Root", "position": [0, 5, 0],
              "children": [ { "name": "Child" } ] },
            { "name": "Sun", "light": { "type": "point", "range": 7 } }
        ]
    })");
    REQUIRE(bp.ok);
    bp.meshes.push_back(makeMesh());
    bp.entities[1].meshes = { 0 };// Child renders the mesh

    entt::registry registry;
    RenderScene scene("test");
    std::vector<entt::entity> created;
    const entt::entity root = instantiate(registry, scene, bp, entt::null, "Mounted", &created);

    REQUIRE(root != entt::null);
    REQUIRE(created.size() == 4);// root + 3 blueprint entities
    CHECK(registry.get<NameComponent>(root).name == "Mounted");

    const entt::entity eRoot = created[1];
    const entt::entity eChild = created[2];
    const entt::entity eSun = created[3];
    CHECK(registry.get<TransformComponent>(eRoot).parent == root);
    CHECK(registry.get<TransformComponent>(eRoot).position.y == Approx(5.0f));
    CHECK(registry.get<TransformComponent>(eChild).parent == eRoot);
    CHECK(registry.get<TransformComponent>(eSun).parent == root);

    REQUIRE(registry.all_of<MeshRendererComponent>(eChild));
    CHECK(registry.get<MeshRendererComponent>(eChild).meshes.size() == 1);
    REQUIRE(registry.all_of<PointLightComponent>(eSun));
    CHECK(registry.get<PointLightComponent>(eSun).radius == Approx(7.0f));

    // Geometry staged exactly once; a second instantiation shares it.
    CHECK(scene.stagedMeshes.size() == 1);
    instantiate(registry, scene, bp);
    CHECK(scene.stagedMeshes.size() == 1);
}

TEST_CASE("instantiate on a bad blueprint is a null no-op", "[scene_blueprint]") {
    entt::registry registry;
    RenderScene scene("test");
    SceneBlueprint bad;// ok == false
    CHECK(instantiate(registry, scene, bad) == entt::null);
    CHECK(registry.storage<entt::entity>().empty());
}
