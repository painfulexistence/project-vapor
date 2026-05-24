#include "resource_manager.hpp"
#include "asset_manager.hpp"
#include <SDL3/SDL.h>
#include <fmt/core.h>
#include <tracy/Tracy.hpp>

namespace Vapor {

    ResourceManager::ResourceManager(TaskScheduler& scheduler) : m_scheduler(scheduler) {
    }

    ResourceManager::~ResourceManager() {
        waitForAll();
    }

    // === Image Loading ===

    auto ResourceManager::loadImage(
        const std::string& path, LoadMode mode, std::function<void(std::shared_ptr<Image>)> onComplete
    ) -> std::shared_ptr<Resource<Image>> {

        return loadResource<Image>(
            path, m_imageCache, [path]() -> auto { return loadImageInternal(path); }, mode, onComplete
        );
    }

    // === Scene Loading ===

    auto ResourceManager::loadScene(
        const std::string& path, bool optimized, LoadMode mode, std::function<void(std::shared_ptr<Scene>)> onComplete
    ) -> std::shared_ptr<Resource<Scene>> {

        // Include optimization flag in cache key
        std::string cacheKey = path + (optimized ? ":optimized" : ":standard");

        return loadResource<Scene>(
            cacheKey,
            m_sceneCache,
            [path, optimized]() -> auto { return loadSceneInternal(path, optimized); },
            mode,
            onComplete
        );
    }

    // === OBJ Loading ===

    auto ResourceManager::loadOBJ(
        const std::string& path,
        const std::string& mtlBasedir,
        LoadMode mode,
        std::function<void(std::shared_ptr<Mesh>)> onComplete
    ) -> std::shared_ptr<Resource<Mesh>> {

        return loadResource<Mesh>(
            path,
            m_meshCache,
            [path, mtlBasedir]() -> auto { return loadMeshInternal(path, mtlBasedir); },
            mode,
            onComplete
        );
    }

    // === Text Loading ===

    auto ResourceManager::loadText(
        const std::string& path, LoadMode mode, std::function<void(std::shared_ptr<std::string>)> onComplete
    ) -> std::shared_ptr<Resource<std::string>> {

        return loadResource<std::string>(
            path,
            m_textCache,
            [path]() -> std::shared_ptr<std::string> { return loadTextInternal(path); },
            mode,
            onComplete
        );
    }

    // === Cache Management ===

    void ResourceManager::clearImageCache() {
        m_imageCache.clear();
    }

    void ResourceManager::clearSceneCache() {
        m_sceneCache.clear();
    }

    void ResourceManager::clearMeshCache() {
        m_meshCache.clear();
    }

    void ResourceManager::clearTextCache() {
        m_textCache.clear();
    }

    void ResourceManager::clearAllCaches() {
        clearImageCache();
        clearSceneCache();
        clearMeshCache();
        clearTextCache();
    }

    auto ResourceManager::getImageCacheSize() const -> size_t {
        return m_imageCache.size();
    }

    auto ResourceManager::getSceneCacheSize() const -> size_t {
        return m_sceneCache.size();
    }

    auto ResourceManager::getMeshCacheSize() const -> size_t {
        return m_meshCache.size();
    }

    auto ResourceManager::getTextCacheSize() const -> size_t {
        return m_textCache.size();
    }

    // === Task Management ===

    void ResourceManager::waitForAll() {
        m_scheduler.waitForAll();
    }

    auto ResourceManager::hasPendingLoads() const -> bool {
        return m_activeLoads.load() > 0;
    }

    auto ResourceManager::getActiveLoadCount() const -> size_t {
        return m_activeLoads.load();
    }

    // === Internal Loading Functions ===

    auto ResourceManager::loadImageInternal(const std::string& path) -> std::shared_ptr<Image> {
        ZoneScoped;
        ZoneName(path.c_str(), path.size());

        return AssetManager::loadImage(path);
    }

    auto ResourceManager::loadSceneInternal(const std::string& path, bool optimized) -> std::shared_ptr<Scene> {
        ZoneScoped;
        ZoneName(path.c_str(), path.size());

        if (optimized) {
            return AssetManager::loadGLTFOptimized(path);
        } else {
            return AssetManager::loadGLTF(path);
        }
    }

    auto ResourceManager::loadMeshInternal(const std::string& path, const std::string& mtlBasedir)
        -> std::shared_ptr<Mesh> {
        ZoneScoped;
        ZoneName(path.c_str(), path.size());

        return AssetManager::loadOBJ(path, mtlBasedir);
    }

    auto ResourceManager::loadTextInternal(const std::string& path) -> std::shared_ptr<std::string> {
        ZoneScoped;
        ZoneName(path.c_str(), path.size());

        // Use SDL3 to load file content
        size_t dataSize = 0;
        void* data = SDL_LoadFile(path.c_str(), &dataSize);
        if (!data) {
            throw std::runtime_error(fmt::format("Failed to load file: {}", path));
        }

        auto text = std::make_shared<std::string>(static_cast<char*>(data), dataSize);
        SDL_free(data);

        return text;
    }

    // === Helper Template Implementation ===

    template<typename T>
    auto ResourceManager::loadResource(
        const std::string& path,
        ResourceCache<T>& cache,
        std::function<std::shared_ptr<T>()> loader,
        LoadMode mode,
        std::function<void(std::shared_ptr<T>)> onComplete
    ) -> std::shared_ptr<Resource<T>> {

        // Check cache first
        auto cached = cache.get(path);
        if (cached) {
            // If there's a callback and resource is ready, call it immediately
            if (onComplete && cached->isReady()) {
                onComplete(cached->get());
            } else if (onComplete) {
                cached->setCallback(onComplete);
            }
            return cached;
        }

        // Create new resource
        auto resource = std::make_shared<Resource<T>>(path);
        resource->setLoading();

        if (onComplete) {
            resource->setCallback(onComplete);
        }

        // Add to cache immediately (even though loading)
        cache.put(path, resource);

        if (mode == LoadMode::Sync) {
            // Synchronous loading
            try {
                auto data = loader();
                resource->setData(data);
            } catch (const std::exception& e) {
                resource->setFailed(e.what());
                fmt::print("Failed to load resource {}: {}\n", path, e.what());
            }
        } else {
            // Asynchronous loading
            m_activeLoads++;

            m_scheduler.submitTask([this, resource, path, loader]() -> auto {
                try {
                    auto data = loader();
                    resource->setData(data);
                } catch (const std::exception& e) {
                    resource->setFailed(e.what());
                    fmt::print("Failed to load resource {}: {}\n", path, e.what());
                }

                m_activeLoads--;
            });
        }

        return resource;
    }

    // Explicit template instantiations
    template std::shared_ptr<Resource<Image>> ResourceManager::loadResource(
        const std::string&,
        ResourceCache<Image>&,
        std::function<std::shared_ptr<Image>()>,
        LoadMode,
        std::function<void(std::shared_ptr<Image>)>
    );

    template std::shared_ptr<Resource<Scene>> ResourceManager::loadResource(
        const std::string&,
        ResourceCache<Scene>&,
        std::function<std::shared_ptr<Scene>()>,
        LoadMode,
        std::function<void(std::shared_ptr<Scene>)>
    );

    template std::shared_ptr<Resource<Mesh>> ResourceManager::loadResource(
        const std::string&,
        ResourceCache<Mesh>&,
        std::function<std::shared_ptr<Mesh>()>,
        LoadMode,
        std::function<void(std::shared_ptr<Mesh>)>
    );

    template std::shared_ptr<Resource<std::string>> ResourceManager::loadResource(
        const std::string&,
        ResourceCache<std::string>&,
        std::function<std::shared_ptr<std::string>()>,
        LoadMode,
        std::function<void(std::shared_ptr<std::string>)>
    );

    // === Atlas Management ===

    AtlasHandle ResourceManager::registerAtlas(const std::string& name, SpriteAtlas atlas) {
        std::lock_guard<std::mutex> lock(m_atlasMutex);

        // Check if already registered
        auto it = m_atlasNameToId.find(name);
        if (it != m_atlasNameToId.end()) {
            // Update existing atlas
            m_atlasMap[it->second] = std::move(atlas);
            return AtlasHandle{ it->second };
        }

        // Register new atlas
        Uint32 id = m_nextAtlasId++;
        atlas.name = name;
        m_atlasMap[id] = std::move(atlas);
        m_atlasNameToId[name] = id;

        return AtlasHandle{ id };
    }

    SpriteAtlas* ResourceManager::getAtlas(AtlasHandle handle) {
        std::lock_guard<std::mutex> lock(m_atlasMutex);
        auto it = m_atlasMap.find(handle.rid);
        return it != m_atlasMap.end() ? &it->second : nullptr;
    }

    const SpriteAtlas* ResourceManager::getAtlas(AtlasHandle handle) const {
        std::lock_guard<std::mutex> lock(m_atlasMutex);
        auto it = m_atlasMap.find(handle.rid);
        return it != m_atlasMap.end() ? &it->second : nullptr;
    }

    AtlasHandle ResourceManager::getAtlasHandle(const std::string& name) const {
        std::lock_guard<std::mutex> lock(m_atlasMutex);
        auto it = m_atlasNameToId.find(name);
        return it != m_atlasNameToId.end() ? AtlasHandle{ it->second } : AtlasHandle{};
    }

    void ResourceManager::removeAtlas(AtlasHandle handle) {
        std::lock_guard<std::mutex> lock(m_atlasMutex);
        auto it = m_atlasMap.find(handle.rid);
        if (it != m_atlasMap.end()) {
            m_atlasNameToId.erase(it->second.name);
            m_atlasMap.erase(it);
        }
    }

    void ResourceManager::clearAtlasCache() {
        std::lock_guard<std::mutex> lock(m_atlasMutex);
        m_atlasMap.clear();
        m_atlasNameToId.clear();
    }

    size_t ResourceManager::getAtlasCacheSize() const {
        std::lock_guard<std::mutex> lock(m_atlasMutex);
        return m_atlasMap.size();
    }

}// namespace Vapor
