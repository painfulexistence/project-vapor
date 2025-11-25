#ifndef ASYNC_ASSET_LOADER_HPP
#define ASYNC_ASSET_LOADER_HPP

#include "task_scheduler.hpp"
#include "graphics.hpp"
#include "scene.hpp"
#include <memory>
#include <string>
#include <functional>
#include <atomic>
#include <mutex>
#include <vector>

namespace Vapor {

/**
 * Asset loading status
 */
enum class AssetLoadStatus {
    Pending,
    Loading,
    Completed,
    Failed
};

/**
 * Base class for async asset loading
 */
template<typename T>
struct AsyncAsset {
    std::shared_ptr<T> data;
    AssetLoadStatus status{AssetLoadStatus::Pending};
    std::string error;
    std::mutex mutex;

    bool isReady() const { return status == AssetLoadStatus::Completed; }
    bool isFailed() const { return status == AssetLoadStatus::Failed; }
    bool isLoading() const { return status == AssetLoadStatus::Loading; }
};

// Type aliases for different asset types
using AsyncImage = AsyncAsset<Image>;
using AsyncScene = AsyncAsset<Scene>;

/**
 * Async asset loader using enkiTS for parallel loading
 */
class AsyncAssetLoader {
public:
    AsyncAssetLoader(TaskScheduler& scheduler);
    ~AsyncAssetLoader();

    // Async image loading
    std::shared_ptr<AsyncImage> loadImageAsync(
        const std::string& filename,
        std::function<void(std::shared_ptr<Image>)> onComplete = nullptr
    );

    // Async GLTF scene loading
    std::shared_ptr<AsyncScene> loadGLTFAsync(
        const std::string& filename,
        bool optimized = true,
        std::function<void(std::shared_ptr<Scene>)> onComplete = nullptr
    );

    // Wait for all pending loads to complete
    void waitForAll();

    // Check if there are any pending loads
    bool hasPendingLoads() const;

    // Get number of active loading tasks
    size_t getActiveLoadCount() const;

private:
    TaskScheduler& m_scheduler;
    std::atomic<size_t> m_activeLoads{0};
    std::vector<std::shared_ptr<AsyncImage>> m_imageAssets;
    std::vector<std::shared_ptr<AsyncScene>> m_sceneAssets;
    std::mutex m_assetsMutex;

    // Internal loading functions (called on worker threads)
    static std::shared_ptr<Image> loadImageInternal(const std::string& filename);
    static std::shared_ptr<Scene> loadGLTFInternal(const std::string& filename, bool optimized);
};

} // namespace Vapor

#endif // ASYNC_ASSET_LOADER_HPP
