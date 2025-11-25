#include "async_asset_loader.hpp"
#include "asset_manager.hpp"
#include <fmt/core.h>

namespace Vapor {

AsyncAssetLoader::AsyncAssetLoader(TaskScheduler& scheduler)
    : m_scheduler(scheduler) {
}

AsyncAssetLoader::~AsyncAssetLoader() {
    waitForAll();
}

std::shared_ptr<AsyncImage> AsyncAssetLoader::loadImageAsync(
    const std::string& filename,
    std::function<void(std::shared_ptr<Image>)> onComplete) {

    auto asyncAsset = std::make_shared<AsyncImage>();
    asyncAsset->status = AssetLoadStatus::Loading;

    {
        std::lock_guard<std::mutex> lock(m_assetsMutex);
        m_imageAssets.push_back(asyncAsset);
    }

    m_activeLoads++;

    // Submit loading task to enkiTS
    m_scheduler.submitTask([this, asyncAsset, filename, onComplete]() {
        try {
            // Load image on worker thread
            auto image = loadImageInternal(filename);

            {
                std::lock_guard<std::mutex> lock(asyncAsset->mutex);
                asyncAsset->data = image;
                asyncAsset->status = AssetLoadStatus::Completed;
            }

            // Call completion callback if provided
            if (onComplete) {
                onComplete(image);
            }
        }
        catch (const std::exception& e) {
            std::lock_guard<std::mutex> lock(asyncAsset->mutex);
            asyncAsset->status = AssetLoadStatus::Failed;
            asyncAsset->error = e.what();
            fmt::print("Failed to load image {}: {}\n", filename, e.what());
        }

        m_activeLoads--;
    });

    return asyncAsset;
}

std::shared_ptr<AsyncScene> AsyncAssetLoader::loadGLTFAsync(
    const std::string& filename,
    bool optimized,
    std::function<void(std::shared_ptr<Scene>)> onComplete) {

    auto asyncAsset = std::make_shared<AsyncScene>();
    asyncAsset->status = AssetLoadStatus::Loading;

    {
        std::lock_guard<std::mutex> lock(m_assetsMutex);
        m_sceneAssets.push_back(asyncAsset);
    }

    m_activeLoads++;

    // Submit loading task to enkiTS
    m_scheduler.submitTask([this, asyncAsset, filename, optimized, onComplete]() {
        try {
            // Load GLTF scene on worker thread
            auto scene = loadGLTFInternal(filename, optimized);

            {
                std::lock_guard<std::mutex> lock(asyncAsset->mutex);
                asyncAsset->data = scene;
                asyncAsset->status = AssetLoadStatus::Completed;
            }

            // Call completion callback if provided
            if (onComplete) {
                onComplete(scene);
            }

            fmt::print("Successfully loaded scene: {}\n", filename);
        }
        catch (const std::exception& e) {
            std::lock_guard<std::mutex> lock(asyncAsset->mutex);
            asyncAsset->status = AssetLoadStatus::Failed;
            asyncAsset->error = e.what();
            fmt::print("Failed to load GLTF {}: {}\n", filename, e.what());
        }

        m_activeLoads--;
    });

    return asyncAsset;
}

void AsyncAssetLoader::waitForAll() {
    m_scheduler.waitForAll();
}

bool AsyncAssetLoader::hasPendingLoads() const {
    return m_activeLoads.load() > 0;
}

size_t AsyncAssetLoader::getActiveLoadCount() const {
    return m_activeLoads.load();
}

// Internal loading functions - these call the existing AssetManager
std::shared_ptr<Image> AsyncAssetLoader::loadImageInternal(const std::string& filename) {
    return AssetManager::loadImage(filename);
}

std::shared_ptr<Scene> AsyncAssetLoader::loadGLTFInternal(const std::string& filename, bool optimized) {
    if (optimized) {
        return AssetManager::loadGLTFOptimized(filename);
    } else {
        return AssetManager::loadGLTF(filename);
    }
}

} // namespace Vapor
