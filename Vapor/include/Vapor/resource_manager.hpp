#ifndef RESOURCE_MANAGER_HPP
#define RESOURCE_MANAGER_HPP

#include "graphics.hpp"
#include "resource.hpp"
#include "resource_cache.hpp"
#include "scene.hpp"
#include "task_scheduler.hpp"
#include <atomic>
#include <memory>
#include <string>
#include <vector>

namespace Vapor {

    /**
     * Modern resource manager with unified sync/async loading
     * Replaces the old AssetManager and AsyncAssetLoader
     */
    class ResourceManager {
    public:
        explicit ResourceManager(TaskScheduler& scheduler);
        ~ResourceManager();

        // === Image Loading ===

        std::shared_ptr<Resource<Image>> loadImage(
            const std::string& path,
            LoadMode mode = LoadMode::Async,
            std::function<void(std::shared_ptr<Image>)> onComplete = nullptr
        );

        // === Scene Loading ===

        std::shared_ptr<Resource<Scene>> loadScene(
            const std::string& path,
            bool optimized = true,
            LoadMode mode = LoadMode::Async,
            std::function<void(std::shared_ptr<Scene>)> onComplete = nullptr
        );

        // === OBJ Loading ===

        std::shared_ptr<Resource<Mesh>> loadOBJ(
            const std::string& path,
            const std::string& mtlBasedir = "",
            LoadMode mode = LoadMode::Async,
            std::function<void(std::shared_ptr<Mesh>)> onComplete = nullptr
        );

        // === Text Loading ===

        std::shared_ptr<Resource<std::string>> loadText(
            const std::string& path,
            LoadMode mode = LoadMode::Async,
            std::function<void(std::shared_ptr<std::string>)> onComplete = nullptr
        );

        // === Cache Management ===

        // Clear specific resource type cache
        void clearImageCache();
        void clearSceneCache();
        void clearMeshCache();
        void clearTextCache();

        // Clear all caches
        void clearAllCaches();

        // Get cache statistics
        size_t getImageCacheSize() const;
        size_t getSceneCacheSize() const;
        size_t getMeshCacheSize() const;
        size_t getTextCacheSize() const;

        // === Task Management ===

        // Wait for all pending loads
        void waitForAll();

        // Check if there are pending loads
        bool hasPendingLoads() const;

        // Get number of active loading tasks
        size_t getActiveLoadCount() const;

    private:
        TaskScheduler& m_scheduler;

        // Resource caches
        ResourceCache<Image> m_imageCache;
        ResourceCache<Scene> m_sceneCache;
        ResourceCache<Mesh> m_meshCache;
        ResourceCache<std::string> m_textCache;

        std::atomic<size_t> m_activeLoads{ 0 };

        // Internal loading functions (static, called on worker threads)
        static std::shared_ptr<Image> loadImageInternal(const std::string& path);
        static std::shared_ptr<Scene> loadSceneInternal(const std::string& path, bool optimized);
        static std::shared_ptr<Mesh> loadMeshInternal(const std::string& path, const std::string& mtlBasedir);
        static std::shared_ptr<std::string> loadTextInternal(const std::string& path);

        // Helper to load resource with caching
        template<typename T>
        std::shared_ptr<Resource<T>> loadResource(
            const std::string& path,
            ResourceCache<T>& cache,
            std::function<std::shared_ptr<T>()> loader,
            LoadMode mode,
            std::function<void(std::shared_ptr<T>)> onComplete
        );
    };

}// namespace Vapor

#endif// RESOURCE_MANAGER_HPP
