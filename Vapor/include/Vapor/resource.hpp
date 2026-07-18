#ifndef RESOURCE_HPP
#define RESOURCE_HPP

#include <memory>
#include <string>
#include <mutex>
#include <atomic>
#include <functional>

namespace Vapor {

/**
 * Resource loading state
 */
enum class ResourceState {
    Unloaded,    // Not yet requested
    Loading,     // Currently loading
    Ready,       // Successfully loaded
    Failed       // Loading failed
};

/**
 * Resource loading mode
 */
enum class LoadMode {
    Sync,        // Block until loaded
    Async        // Load in background
};

/**
 * Generic resource container with loading state tracking
 *
 * Template parameter T should be the resource type (Image, Scene, Mesh, etc.)
 */
template<typename T>
class Resource {
public:
    Resource() = default;

    explicit Resource(const std::string& path)
        : m_path(path)
        , m_state(ResourceState::Unloaded) {}

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
    std::atomic<ResourceState> m_state{ResourceState::Unloaded};
    std::string m_error;
    std::function<void(std::shared_ptr<T>)> m_callback;

    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
};

} // namespace Vapor

#endif // RESOURCE_HPP
