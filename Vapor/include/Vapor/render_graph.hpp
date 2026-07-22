#pragma once
#include "rhi.hpp"// RHICapabilities
#include <SDL3/SDL_stdinc.h>
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Vapor {

class Renderer;

// ============================================================================
// RenderGraph — the ordered list of render passes executed each frame by
// Renderer::render().
//
// Passes are backend-agnostic: each pass declares the hardware capabilities
// it requires (PassFlags), and the graph automatically skips passes that the
// active RHI backend cannot satisfy. This is how single-backend features are
// expressed — e.g. a raytraced-shadow pass runs on Metal (which reports
// RHICapabilities::raytracing) and is silently skipped on Vulkan, with no
// backend checks anywhere in pass or gameplay code.
//
// The gameplay layer owns the composition: it can append CallbackPass
// lambdas, insert custom RenderPass subclasses, remove or reorder built-in
// passes, and toggle passes at runtime via Renderer::getRenderGraph().
// ============================================================================

// Capability requirements a pass declares. Combine with operator|.
enum class PassFlags : Uint32 {
    None = 0,
    // Uses acceleration structures / ray queries; skipped when
    // RHICapabilities::raytracing is false.
    RequiresRaytracing = 1 << 0,
    // Dispatches compute work; skipped when RHICapabilities::computeShaders
    // is false.
    RequiresCompute = 1 << 1,
};

inline PassFlags operator|(PassFlags a, PassFlags b) {
    return static_cast<PassFlags>(static_cast<Uint32>(a) | static_cast<Uint32>(b));
}

inline bool hasFlag(PassFlags value, PassFlags flag) {
    return (static_cast<Uint32>(value) & static_cast<Uint32>(flag)) != 0;
}

class RenderPass {
public:
    explicit RenderPass(std::string name, PassFlags flags = PassFlags::None)
      : m_name(std::move(name)), m_flags(flags) {
    }
    virtual ~RenderPass() = default;

    virtual void execute(Renderer& renderer) = 0;

    const std::string& getName() const { return m_name; }
    PassFlags getFlags() const { return m_flags; }

    // True when the backend satisfies every capability this pass requires.
    bool isSupported(const RHICapabilities& caps) const {
        if (hasFlag(m_flags, PassFlags::RequiresRaytracing) && !caps.raytracing) return false;
        if (hasFlag(m_flags, PassFlags::RequiresCompute) && !caps.computeShaders) return false;
        return true;
    }

    // Runtime toggle (e.g. from the Engine inspector or gameplay logic).
    bool enabled = true;

private:
    std::string m_name;
    PassFlags m_flags;
};

// Pass defined by a callable — the usual way gameplay code defines passes:
//   renderer->getRenderGraph().addPass("Outline", [](Renderer& r) { ... });
class CallbackPass : public RenderPass {
public:
    CallbackPass(std::string name, PassFlags flags, std::function<void(Renderer&)> fn)
      : RenderPass(std::move(name), flags), m_fn(std::move(fn)) {
    }

    void execute(Renderer& renderer) override {
        if (m_fn) m_fn(renderer);
    }

private:
    std::function<void(Renderer&)> m_fn;
};

class RenderGraph {
public:
    RenderPass* addPass(std::unique_ptr<RenderPass> pass) {
        passes.push_back(std::move(pass));
        return passes.back().get();
    }

    RenderPass* addPass(std::string name, std::function<void(Renderer&)> fn, PassFlags flags = PassFlags::None) {
        return addPass(std::make_unique<CallbackPass>(std::move(name), flags, std::move(fn)));
    }

    RenderPass* findPass(std::string_view name) {
        for (auto& pass : passes) {
            if (pass->getName() == name) return pass.get();
        }
        return nullptr;
    }

    bool removePass(std::string_view name) {
        for (auto it = passes.begin(); it != passes.end(); ++it) {
            if ((*it)->getName() == name) {
                passes.erase(it);
                return true;
            }
        }
        return false;
    }

    void clear() {
        passes.clear();
    }

    // Executes enabled passes in order, skipping any whose declared
    // requirements the backend does not meet.
    void execute(Renderer& renderer, const RHICapabilities& caps) {
        m_passCpuTimings.clear();
        for (auto& pass : passes) {
            if (!pass->enabled) continue;
            if (!pass->isSupported(caps)) continue;
            // Per-pass CPU (command-recording) time. This is wall-clock time on
            // the calling thread — how long the CPU spends building the pass,
            // NOT its GPU execution time (see RHI::getGpuPassTimings for that).
            // Comparing the two tells you whether a pass is CPU- or GPU-bound.
            // Publish the pass name so passes can label their GPU-timing
            // compute/render scope with it (beginComputePass) instead of a
            // parallel hardcoded string that silently drifts from this one.
            m_activePassName = pass->getName();
            const auto t0 = std::chrono::high_resolution_clock::now();
            pass->execute(renderer);
            const auto t1 = std::chrono::high_resolution_clock::now();
            m_passCpuTimings.emplace_back(
                pass->getName(),
                std::chrono::duration<double, std::milli>(t1 - t0).count());
        }
    }

    // Per-pass CPU recording times from the last execute(), in pass order.
    const std::vector<std::pair<std::string, double>>& getPassCpuTimings() const {
        return m_passCpuTimings;
    }

    const std::vector<std::unique_ptr<RenderPass>>& getPasses() const {
        return passes;
    }

    // Name of the pass currently executing (valid during execute()). A pass
    // uses it to label its GPU-timing scope so the profiler entry tracks the
    // pass name automatically. Empty outside execute().
    const std::string& activePassName() const { return m_activePassName; }

private:
    std::vector<std::unique_ptr<RenderPass>> passes;
    std::vector<std::pair<std::string, double>> m_passCpuTimings;
    std::string m_activePassName;
};

} // namespace Vapor

// Transitional shim: these types lived at global scope before the namespace
// unification; unqualified call sites keep compiling while they migrate to
// Vapor:: qualification. Remove once call sites are migrated.
using namespace Vapor;
