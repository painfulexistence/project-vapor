#pragma once

#include "graphics.hpp"
#include "scene.hpp"
#include "task_scheduler.hpp"
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace Vapor {

    enum class ResourceState {
        Unloaded,// Not yet requested
        Loading,// Currently loading
        Ready,// Successfully loaded
        Failed// Loading failed
    };

    /**
     * Resource loading mode
     */
    enum class LoadMode {
        Sync,// Block until loaded
        Async// Load in background
    };

    /**
     * Generic resource container with loading state tracking
     *
     * Template parameter T should be the resource type (Image, Scene, Mesh, etc.)
     */
    template<typename T> class Resource {
    public:
        Resource() = default;

        explicit Resource(const std::string& path) : m_path(path), m_state(ResourceState::Unloaded) {
        }

        // Get the underlying resource data (blocks if loading)
        std::shared_ptr<T> get() {
            // Wait for loading to complete
            std::unique_lock<std::mutex> lock(m_mutex);
            while (m_state == ResourceState::Loading) {
                m_cv.wait(lock);
            }
            return m_data;
        }

        // Try to get resource without blocking
        std::shared_ptr<T> tryGet() const {
            std::lock_guard<std::mutex> lock(m_mutex);
            return m_data;
        }

        // Check if resource is ready
        bool isReady() const {
            return m_state.load() == ResourceState::Ready;
        }

        // Check if resource failed to load
        bool isFailed() const {
            return m_state.load() == ResourceState::Failed;
        }

        // Check if resource is currently loading
        bool isLoading() const {
            return m_state.load() == ResourceState::Loading;
        }

        // Get current state
        ResourceState getState() const {
            return m_state.load();
        }

        // Get resource path
        const std::string& getPath() const {
            return m_path;
        }

        // Get error message (if failed)
        const std::string& getError() const {
            std::lock_guard<std::mutex> lock(m_mutex);
            return m_error;
        }

        // Set completion callback
        void setCallback(std::function<void(std::shared_ptr<T>)> callback) {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_callback = callback;
        }

        // Internal: Set resource data (called by loader)
        void setData(std::shared_ptr<T> data) {
            std::function<void(std::shared_ptr<T>)> callback;

            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_data = data;
                m_state.store(ResourceState::Ready);
                callback = m_callback;
            }

            m_cv.notify_all();

            // Call callback outside of lock
            if (callback) {
                callback(data);
            }
        }

        // Internal: Set loading state
        void setLoading() {
            m_state.store(ResourceState::Loading);
        }

        // Internal: Set failed state with error
        void setFailed(const std::string& error) {
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_error = error;
                m_state.store(ResourceState::Failed);
            }

            m_cv.notify_all();
        }

    private:
        std::string m_path;
        std::shared_ptr<T> m_data;
        std::atomic<ResourceState> m_state{ ResourceState::Unloaded };
        std::string m_error;
        std::function<void(std::shared_ptr<T>)> m_callback;

        mutable std::mutex m_mutex;
        std::condition_variable m_cv;
    };

    /**
     * Thread-safe resource cache
     * Manages loaded resources and prevents duplicate loading
     */
    template<typename T> class ResourceCache {
    public:
        ResourceCache() = default;

        // Try to get cached resource
        std::shared_ptr<Resource<T>> get(const std::string& path) {
            std::lock_guard<std::mutex> lock(m_mutex);
            auto it = m_cache.find(path);
            if (it != m_cache.end()) {
                return it->second;
            }
            return nullptr;
        }

        // Store resource in cache
        void put(const std::string& path, std::shared_ptr<Resource<T>> resource) {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_cache[path] = resource;
        }

        // Check if resource is cached
        bool contains(const std::string& path) const {
            std::lock_guard<std::mutex> lock(m_mutex);
            return m_cache.find(path) != m_cache.end();
        }

        // Remove resource from cache
        void remove(const std::string& path) {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_cache.erase(path);
        }

        // Clear all cached resources
        void clear() {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_cache.clear();
        }

        // Get number of cached resources
        size_t size() const {
            std::lock_guard<std::mutex> lock(m_mutex);
            return m_cache.size();
        }

        // Get memory usage estimate (in bytes)
        size_t getMemoryUsage() const {
            std::lock_guard<std::mutex> lock(m_mutex);

            size_t total = 0;
            for (const auto& [path, resource] : m_cache) {
                if (resource->isReady()) {
                    auto data = resource->tryGet();
                    if (data) {
                        total += estimateSize(data);
                    }
                }
            }
            return total;
        }

    private:
        mutable std::mutex m_mutex;
        std::unordered_map<std::string, std::shared_ptr<Resource<T>>> m_cache;

        // Helper to estimate resource size (can be specialized for different types)
        size_t estimateSize(const std::shared_ptr<T>& data) const;
    };


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

        // === Atlas Management ===

        // Register a sprite atlas and return its handle
        AtlasHandle registerAtlas(const std::string& name, SpriteAtlas atlas);

        // Get atlas by handle (returns nullptr if not found)
        SpriteAtlas* getAtlas(AtlasHandle handle);
        const SpriteAtlas* getAtlas(AtlasHandle handle) const;

        // Get atlas by name (returns invalid handle if not found)
        AtlasHandle getAtlasHandle(const std::string& name) const;

        // Remove atlas
        void removeAtlas(AtlasHandle handle);

        // Clear all atlases
        void clearAtlasCache();

        // Get atlas count
        size_t getAtlasCacheSize() const;

    private:
        TaskScheduler& m_scheduler;

        // Resource caches
        ResourceCache<Image> m_imageCache;
        ResourceCache<Scene> m_sceneCache;
        ResourceCache<Mesh> m_meshCache;
        ResourceCache<std::string> m_textCache;

        // Atlas storage (handle-based, not async)
        std::unordered_map<Uint32, SpriteAtlas> m_atlasMap;
        std::unordered_map<std::string, Uint32> m_atlasNameToId;
        Uint32 m_nextAtlasId = 0;
        mutable std::mutex m_atlasMutex;

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