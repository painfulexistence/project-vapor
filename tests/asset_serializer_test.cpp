#include "Vapor/asset_serializer.hpp"
#include "Vapor/graphics.hpp"
#include "Vapor/scene_blueprint.hpp"
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cereal/archives/binary.hpp>
#include <cereal/types/string.hpp>
#include <cereal/types/vector.hpp>
#include <fstream>
#include <glm/glm.hpp>
#include <memory>
#include <sstream>

using namespace Vapor;

struct TestData {
    std::string name;
    glm::vec3 position;
    glm::mat4 transform;
    std::vector<int> numbers;

    template<class Archive> void serialize(Archive& archive) {
        archive(name, position, transform, numbers);
    }
};

TEST_CASE("AssetSerializer - Simple Cereal Test", "[model][serializer][cereal]") {
    TestData data;
    data.name = "Test";
    data.position = glm::vec3(1.0f, 2.0f, 3.0f);
    data.transform = glm::mat4(1.0f);
    data.numbers = { 1, 2, 3, 4, 5 };

    std::string testPath = "test_simple.bin";
    {
        std::ofstream file(testPath, std::ios::binary);
        cereal::BinaryOutputArchive archive(file);
        archive(data);
    }

    TestData loadedData;
    {
        std::ifstream file(testPath, std::ios::binary);
        cereal::BinaryInputArchive archive(file);
        archive(loadedData);
    }

    CHECK(loadedData.name == "Test");
    CHECK(loadedData.numbers.size() == 5);
    CHECK(loadedData.position.x == 1.0f);

    std::remove(testPath.c_str());
}

// The .vscene cook is the live scene cache; it (de)serializes a SceneBlueprint via
// AssetSerializer::(de)serializeBlueprint, which shares (de)serializeMesh. This
// round-trips a mesh WITH baked meshletData — exactly the field set that, when its
// version wasn't bumped, misparsed a stale cook and crashed the loader.
TEST_CASE("AssetSerializer - blueprint mesh round-trip preserves meshletData",
          "[asset][serializer][blueprint]") {
    SceneBlueprint bp;
    bp.name = "RoundTrip";

    auto mesh = std::make_shared<Mesh>();
    mesh->vertices = {
        { { -1.0f, -1.0f, 0.0f }, { 0.0f, 0.0f }, { 0.0f, 0.0f, 1.0f }, { 1.0f, 0.0f, 0.0f, 1.0f } },
        { { 1.0f, -1.0f, 0.0f }, { 1.0f, 0.0f }, { 0.0f, 0.0f, 1.0f }, { 1.0f, 0.0f, 0.0f, 1.0f } },
        { { 0.0f, 1.0f, 0.0f }, { 0.5f, 1.0f }, { 0.0f, 0.0f, 1.0f }, { 1.0f, 0.0f, 0.0f, 1.0f } },
    };
    mesh->indices = { 0, 1, 2 };
    mesh->vertexCount = 3;
    mesh->indexCount = 3;
    mesh->primitiveMode = PrimitiveMode::TRIANGLES;

    Meshlet m{};
    m.vertexOffset = 0;
    m.triangleOffset = 0;
    m.vertexCount = 3;
    m.triangleCount = 1;
    mesh->meshletData.meshlets.push_back(m);
    mesh->meshletData.meshletVertices = { 0, 1, 2 };
    mesh->meshletData.meshletTriangles = { 0, 1, 2 };
    mesh->meshletData.lodLevelCount = 1;

    bp.meshes.push_back(mesh);

    std::stringstream ss(std::ios::in | std::ios::out | std::ios::binary);
    {
        cereal::BinaryOutputArchive out(ss);
        AssetSerializer::serializeBlueprint(out, bp);
    }
    SceneBlueprint loaded;
    {
        cereal::BinaryInputArchive in(ss);
        loaded = AssetSerializer::deserializeBlueprint(in);
    }

    REQUIRE(loaded.ok);
    REQUIRE(loaded.meshes.size() == 1);
    REQUIRE(loaded.meshes[0] != nullptr);
    CHECK(loaded.meshes[0]->indices.size() == 3);
    CHECK(loaded.meshes[0]->vertices.size() == 3);
    REQUIRE(loaded.meshes[0]->meshletData.meshlets.size() == 1);
    CHECK(loaded.meshes[0]->meshletData.meshlets[0].vertexCount == 3u);
    CHECK(loaded.meshes[0]->meshletData.meshlets[0].triangleCount == 1u);
    CHECK(loaded.meshes[0]->meshletData.meshletVertices.size() == 3);
    CHECK(loaded.meshes[0]->meshletData.meshletTriangles.size() == 3);
    CHECK(loaded.meshes[0]->meshletData.lodLevelCount == 1u);
}

TEST_CASE("AssetSerializer - blueprint version mismatch yields ok == false",
          "[asset][serializer][blueprint]") {
    std::stringstream ss(std::ios::in | std::ios::out | std::ios::binary);
    {
        cereal::BinaryOutputArchive out(ss);
        uint32_t badVersion = 0xDEADBEEFu;// deliberately not BLUEPRINT_FORMAT_VERSION
        out(badVersion);
    }
    cereal::BinaryInputArchive in(ss);
    SceneBlueprint bp = AssetSerializer::deserializeBlueprint(in);
    CHECK_FALSE(bp.ok);
}
