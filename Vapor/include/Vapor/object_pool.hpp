#ifndef OBJECT_POOL_HPP
#define OBJECT_POOL_HPP

#include <vector>
#include <mutex>
#include <condition_variable>
#include <memory>
#include <stdexcept>

namespace Vapor {

/**
 * A simple thread-safe, blocking object pool.
 * @tparam T The type of object to pool.
 */
template<typename T>
class ObjectPool {
public:
    explicit ObjectPool(size_t size) : m_pool(size) {
        for (size_t i = 0; i < size; ++i) {
            m_free_indices.push_back(i);
        }
    }

    // Acquires an object from the pool. Blocks if the pool is empty.
    T* acquire() {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_cv.wait(lock, [this] { return !m_free_indices.empty(); });
        
        size_t index = m_free_indices.back();
        m_free_indices.pop_back();
        
        return &m_pool[index];
    }

    // Releases an object back to the pool.
    void release(T* obj) {
        std::lock_guard<std::mutex> lock(m_mutex);
        
        // simple way to get index back from pointer
        size_t index = obj - m_pool.data();
        
        m_free_indices.push_back(index);
        m_cv.notify_one();
    }

private:
    std::vector<T> m_pool;
    std::vector<size_t> m_free_indices;
    std::mutex m_mutex;
    std::condition_variable m_cv;
};

} // namespace Vapor

#endif // OBJECT_POOL_HPP
