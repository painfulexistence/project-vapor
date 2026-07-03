#pragma once
// [FIXTURE: design]
// Each issue is tagged [ISSUE:DES-XXX]. Run: /cpp-review design --dir .claude/commands/cpp-review-fixtures
// Expected: all DES-xxx found; OK sections must NOT be flagged.

#include <memory>
#include <optional>
#include <stdexcept>
#include <string>

// ── Singleton pattern inconsistency ──────────────────────────────────────────

// [ISSUE:DES-001a] Singleton style A: raw pointer static member, returns raw ptr
class AudioSystem {
public:
    static AudioSystem* s_instance;
    static AudioSystem* Get() { return s_instance; }  // raw pointer
    void playSound(const std::string& path) {}
};

// [ISSUE:DES-001b] Singleton style B: Meyers singleton, returns reference
//                  Two different singleton patterns in the same codebase
class PhysicsSystem {
public:
    static PhysicsSystem& Get() {
        static PhysicsSystem instance;
        return instance;
    }
    void step(float dt) {}
};

// ── Error handling strategy inconsistency ────────────────────────────────────

// [ISSUE:DES-002a] Manager A: throws on failure
class SceneManager {
public:
    void load(const std::string& path) {
        if (path.empty())
            throw std::runtime_error("empty path");  // throws
    }
    void unload() {}
};

// [ISSUE:DES-002b] Manager B: returns bool
class AssetManager {
public:
    bool load(const std::string& path) {   // returns bool
        return !path.empty();
    }
    void unload() {}
};

// [ISSUE:DES-002c] Manager C: returns optional
class FontManager {
public:
    std::optional<int> load(const std::string& path) {  // optional
        if (path.empty()) return std::nullopt;
        return 42;
    }
    void unload() {}
};

// OK — consistent error handling within one subsystem (all return bool)
class NetworkManager {
public:
    bool connect(const std::string& host) { return !host.empty(); }
    bool send(const void* data, std::size_t n) { return n > 0; }
    bool disconnect() { return true; }
};

// ── API asymmetry ─────────────────────────────────────────────────────────────

// [ISSUE:DES-003] init() exists but deinit() is missing; pause() with no resume()
class RenderSystem {
public:
    void init();    // has init —
                    // [DES-003a] missing deinit()
    void pause();   // [DES-003b] missing resume()
    void draw();
};

// OK — symmetric init/deinit pair
class InputManager {
public:
    void init();
    void deinit();
    void update(float dt);
};

// ── Ownership model mixing ────────────────────────────────────────────────────

// [ISSUE:DES-004] Same conceptual resource (texture handle) managed three different
//                 ways within the same class — raw ptr, shared_ptr, and integer ID
class MaterialSystem {
public:
    // Three different ownership models for logically the same kind of resource:
    unsigned int m_albedoId    = 0;               // raw integer ID
    void*        m_normalMap   = nullptr;          // raw owning pointer
    std::shared_ptr<unsigned int> m_roughnessMap;  // shared_ptr to an int?

    void bind() {}
};

// ── Abstract interface bypass ─────────────────────────────────────────────────

// [ISSUE:DES-005] Derived class exposes concrete methods not in the base interface,
//                 allowing callers to bypass the abstraction if they have a concrete ptr
class RendererBase {
public:
    virtual ~RendererBase() = default;
    virtual void init()  = 0;
    virtual void draw()  = 0;
    virtual void deinit() = 0;
};

class RendererVulkan : public RendererBase {
public:
    void init()   override {}
    void draw()   override {}
    void deinit() override {}

    // [DES-005] Public Vulkan-specific methods not in RendererBase
    //           Callers holding RendererVulkan* bypass the abstraction
    void createPipeline(const std::string& vert, const std::string& frag) {}
    void createComputePipeline(const std::string& shader) {}
    void rebuildSwapchain() {}
};
