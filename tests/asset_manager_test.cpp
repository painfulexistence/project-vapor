#include "Vapor/asset_manager.hpp"
#include "Vapor/file_system.hpp"
#include <catch2/catch_test_macros.hpp>

TEST_CASE("asset manager - model loading", "[assets]") {
    FileSystem::instance().initialize();
    std::string modelPath = "models/cube.obj";
    if (!FileSystem::instance().resolvePath(modelPath)) {
        SKIP("Test model not found: " << modelPath);
    }

    try {
        AssetManager::loadOBJ(modelPath);
        SUCCEED("Model loaded successfully");
    } catch (const std::exception& e) {
        FAIL("Failed to load existing model: " << e.what());
    }
}
