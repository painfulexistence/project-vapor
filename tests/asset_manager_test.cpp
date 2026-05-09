#include "Vapor/asset_manager.hpp"
#include <catch2/catch_test_macros.hpp>
#include <filesystem>

TEST_CASE("asset manager - model loading", "[assets]") {
    std::string modelPath = "assets/models/cube.obj";
    if (!std::filesystem::exists(modelPath)) {
        SKIP("Test model not found: " << modelPath);
    }

    try {
        AssetManager::loadOBJ(modelPath);
        SUCCEED("Model loaded successfully");
    } catch (const std::exception& e) {
        FAIL("Failed to load existing model: " << e.what());
    }
}
