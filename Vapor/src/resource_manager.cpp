#include "resource_manager.hpp"
#include "asset_manager.hpp"
#include <fmt/core.h>
#include <tracy/Tracy.hpp>

namespace Vapor {

ResourceManager::ResourceManager(TaskScheduler& scheduler)
    : m_scheduler(scheduler) {
}

ResourceManager::~ResourceManager() {
    waitForAll();
}

// === Image Loading ===

std::shared_ptr<Resource<Image>> ResourceManager::loadImage(
    const std::string& path,
    LoadMode mode,
    std::function<void(std::shared_ptr<Image>)> onComplete) {

    return loadResource<Image>(
        path,
        m_imageCache,
        [path]() { return loadImageInternal(path); },
        mode,
        onComplete
    );
}

// === Scene Loading ===

std::shared_ptr<Resource<Scene>> ResourceManager::loadScene(
    const std::string& path,
    bool optimized,
    LoadMode mode,
    std::function<void(std::shared_ptr<Scene>)> onComplete) {

    // Include optimization flag in cache key
    std::string cacheKey = path + (optimized ? ":optimized" : ":standard");

    return loadResource<Scene>(
        cacheKey,
        m_sceneCache,
        [path, optimized]() { return loadSceneInternal(path, optimized); },
        mode,
        onComplete
    );
}

// === OBJ Loading ===

std::shared_ptr<Resource<Mesh>> ResourceManager::loadOBJ(
    const std::string& path,
    const std::string& mtlBasedir,
    LoadMode mode,
    std::function<void(std::shared_ptr<Mesh>)> onComplete) {

    return loadResource<Mesh>(
        path,
        m_meshCache,
        [path, mtlBasedir]() { return loadMeshInternal(path, mtlBasedir); },
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

void ResourceManager::clearAllCaches() {
    clearImageCache();
    clearSceneCache();
    clearMeshCache();
}

size_t ResourceManager::getImageCacheSize() const {
    return m_imageCache.size();
}

size_t ResourceManager::getSceneCacheSize() const {
    return m_sceneCache.size();
}

size_t ResourceManager::getMeshCacheSize() const {
    return m_meshCache.size();
}

// === Task Management ===

void ResourceManager::waitForAll() {
    m_scheduler.waitForAll();
}

bool ResourceManager::hasPendingLoads() const {
    return m_activeLoads.load() > 0;
}

size_t ResourceManager::getActiveLoadCount() const {
    return m_activeLoads.load();
}

// === Internal Loading Functions ===

std::shared_ptr<Image> ResourceManager::loadImageInternal(const std::string& path) {
    ZoneScoped;
    ZoneName(path.c_str(), path.size());

    return AssetManager::loadImage(path);
}

std::shared_ptr<Scene> ResourceManager::loadSceneInternal(const std::string& path, bool optimized) {
    ZoneScoped;
    ZoneName(path.c_str(), path.size());

    if (optimized) {
        return AssetManager::loadGLTFOptimized(path);
    } else {
        return AssetManager::loadGLTF(path);
    }
}

std::shared_ptr<Mesh> ResourceManager::loadMeshInternal(const std::string& path, const std::string& mtlBasedir) {
    ZoneScoped;
    ZoneName(path.c_str(), path.size());

    return AssetManager::loadOBJ(path, mtlBasedir);
}

// === Helper Template Implementation ===

template<typename T>
std::shared_ptr<Resource<T>> ResourceManager::loadResource(
    const std::string& path,
    ResourceCache<T>& cache,
    std::function<std::shared_ptr<T>()> loader,
    LoadMode mode,
    std::function<void(std::shared_ptr<T>)> onComplete) {

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
        }
        catch (const std::exception& e) {
            resource->setFailed(e.what());
            fmt::print("Failed to load resource {}: {}\n", path, e.what());
        }
    }
    else {
        // Asynchronous loading
        m_activeLoads++;

        m_scheduler.submitTask([this, resource, path, loader]() {
            try {
                auto data = loader();
                resource->setData(data);
            }
            catch (const std::exception& e) {
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
    const std::string&, ResourceCache<Image>&,
    std::function<std::shared_ptr<Image>()>, LoadMode,
    std::function<void(std::shared_ptr<Image>)>);

template std::shared_ptr<Resource<Scene>> ResourceManager::loadResource(
    const std::string&, ResourceCache<Scene>&,
    std::function<std::shared_ptr<Scene>()>, LoadMode,
    std::function<void(std::shared_ptr<Scene>)>);

template std::shared_ptr<Resource<Mesh>> ResourceManager::loadResource(
    const std::string&, ResourceCache<Mesh>&,
    std::function<std::shared_ptr<Mesh>()>, LoadMode,
    std::function<void(std::shared_ptr<Mesh>)>);

} // namespace Vapor
