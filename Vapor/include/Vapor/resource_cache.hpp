#ifndef RESOURCE_CACHE_HPP
#define RESOURCE_CACHE_HPP

#include "resource.hpp"
#include <unordered_map>
#include <memory>
#include <mutex>
#include <string>

namespace Vapor {

/**
 * Thread-safe resource cache
 * Manages loaded resources and prevents duplicate loading
 */
template<typename T>
class ResourceCache {
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

} // namespace Vapor

#endif // RESOURCE_CACHE_HPP
