#include <catch2/catch_test_macros.hpp>
#include "Vapor/asset_serializer.hpp"
#include "Vapor/graphics.hpp"
#include "Vapor/scene.hpp"
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <iostream>
#include <memory>
#include <fstream>
#include <cereal/archives/binary.hpp>
#include <cereal/types/string.hpp>
#include <cereal/types/vector.hpp>

using namespace Vapor;

TEST_CASE("AssetSerializer - Scene Serialization", "[asset][serializer]") {
    // Create a test scene
    auto scene = std::make_shared<Scene>("TestScene");

    // Create a test image
    auto image = std::make_shared<Image>();
    image->uri = "test_texture.png";
    image->width = 256;
    image->height = 256;
    image->channelCount = 4;
    image->byteArray = std::vector<Uint8>(256 * 256 * 4, 128);
    scene->images.push_back(image);

    // Create a test material
    auto material = std::make_shared<Material>();
    material->name = "TestMaterial";
    material->alphaMode = AlphaMode::OPAQUE;
    material->alphaCutoff = 0.5f;
    material->doubleSided = false;
    material->baseColorFactor = glm::vec4(1.0f, 0.5f, 0.2f, 1.0f);
    material->normalScale = 1.0f;
    material->metallicFactor = 0.0f;
    material->roughnessFactor = 0.5f;
    material->occlusionStrength = 1.0f;
    material->emissiveFactor = glm::vec3(0.0f);
    material->albedoMap = image;
    scene->materials.push_back(material);

    // Create a test mesh
    auto mesh = std::make_shared<Mesh>();
    mesh->vertices = { { { -1.0f, -1.0f, 0.0f }, { 0.0f, 0.0f }, { 0.0f, 0.0f, 1.0f }, { 1.0f, 0.0f, 0.0f, 1.0f } },
                       { { 1.0f, -1.0f, 0.0f }, { 1.0f, 0.0f }, { 0.0f, 0.0f, 1.0f }, { 1.0f, 0.0f, 0.0f, 1.0f } },
                       { { 0.0f, 1.0f, 0.0f }, { 0.5f, 1.0f }, { 0.0f, 0.0f, 1.0f }, { 1.0f, 0.0f, 0.0f, 1.0f } } };
    mesh->indices = { 0, 1, 2 };
    mesh->vertexCount = 3;
    mesh->indexCount = 3;
    mesh->material = material;
    mesh->primitiveMode = PrimitiveMode::TRIANGLES;

    // Create a test node
    auto node = scene->createNode("TestNode", glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, -5.0f)));
    scene->addMeshToNode(node, mesh);

    // Add a directional light
    DirectionalLight dirLight;
    dirLight.direction = glm::vec3(0.0f, -1.0f, 0.0f);
    dirLight.color = glm::vec3(1.0f, 1.0f, 1.0f);
    dirLight.intensity = 1.0f;
    scene->directionalLights.push_back(dirLight);

    // Test serialization
    std::string testPath = "test_scene.bin";
    AssetSerializer::serializeScene(scene, testPath);

    // Test deserialization
    auto loadedScene = AssetSerializer::deserializeScene(testPath);

    REQUIRE(loadedScene != nullptr);
    CHECK(loadedScene->name == "TestScene");
    CHECK(loadedScene->images.size() == 1);
    CHECK(loadedScene->materials.size() == 1);
    CHECK(loadedScene->directionalLights.size() == 1);
    
    // Cleanup
    std::remove(testPath.c_str());
}

struct TestData {
    std::string name;
    glm::vec3 position;
    glm::mat4 transform;
    std::vector<int> numbers;

    template<class Archive> void serialize(Archive& archive) {
        archive(name, position, transform, numbers);
    }
};

TEST_CASE("AssetSerializer - Simple Cereal Test", "[asset][serializer][cereal]") {
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
