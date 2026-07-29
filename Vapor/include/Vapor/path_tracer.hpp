#pragma once
#include "render_data.hpp"
#include "rhi.hpp"
#include <glm/mat4x4.hpp>

namespace Vapor {

// ============================================================================
// Progressive path tracer — the photo-mode integrator.
//
// This is a renderer of its own, not an effect bolted onto the raster frame:
// it generates camera rays, traces them against the scene TLAS, and sums
// radiance into an accumulator that converges as long as the view holds still.
//
// It deliberately knows nothing about Renderer. Everything it reads from the
// frame arrives through PathTraceSceneView, which the caller fills from
// resources that already exist for the raster and RT paths. That one-way
// contract is what keeps photo mode from leaving traces in the scene layer —
// and what will let a headless CLI renderer drive the same integrator without
// a swapchain or a game loop.
// ============================================================================

// Everything the integrator reads from the frame's scene state. All handles
// are borrowed: the path tracer never creates, destroys, or writes any of them.
struct PathTraceSceneView {
    AccelStructHandle tlas;
    BufferHandle camera;              // CameraRenderData
    BufferHandle instances;           // InstanceData[]
    BufferHandle materials;           // MaterialData[]
    BufferHandle directionalLights;   // DirectionalLightData[]
    size_t instanceRange = 0;
    size_t materialRange = 0;
    size_t directionalLightRange = 0;
    Uint32 directionalLightCount = 0;

    // Merged scene geometry + the bindless per-material texture table. Hits
    // resolve their normal, UV and textures through these; without them the
    // integrator can only shade with material factors (see hasGeometry).
    BufferHandle mergedVertices;
    BufferHandle mergedIndices;
    BufferHandle materialTextureTable;

    TextureHandle environment;        // prefiltered environment cube (miss + ambient)

    // True when the merged geometry and material table are all bound, so hits
    // can fetch interpolated normals, UVs and material textures.
    bool hasGeometry() const {
        return mergedVertices.isValid() && mergedIndices.isValid() && materialTextureTable.isValid();
    }

    // Minimum needed to trace at all. The light buffer is required even for an
    // unlit scene: the kernel declares the argument unconditionally and uses
    // directionalLightCount, not the binding, to decide whether to read it.
    bool isTraceable() const {
        return tlas.isValid() && camera.isValid() && instances.isValid() &&
               materials.isValid() && directionalLights.isValid() && environment.isValid();
    }
};

struct PathTraceSettings {
    // Bounces AFTER the primary ray. 0 = direct lighting only (no indirect);
    // 4 already carries most of the visible interreflection in a closed room.
    Uint32 maxBounces = 4;
    // Samples per pixel per frame. 1 keeps the UI responsive while the image
    // refines; raising it converges in fewer frames but costs proportionally.
    Uint32 samplesPerFrame = 1;
    // Accumulation stops here. The image stays on screen; the GPU goes idle.
    Uint32 maxSamples = 4096;
    // Sun disk half-angle. The real sun is ~0.0047 rad; 0 gives a hard shadow.
    float sunAngularRadius = 0.0047f;
    float rayBias = 0.001f;
    float rayMaxDistance = 10000.0f;
    float envIntensity = 1.0f;
    // Per-sample radiance ceiling. <= 0 disables clamping (unbiased, but leaves
    // stuck bright pixels that take very many samples to average out).
    float fireflyClamp = 20.0f;
};

// Progressive accumulation state: how many samples the current image holds,
// and when that image has to be thrown away. Pure logic — no GPU state — so
// the invalidation rules can be tested without a device.
class PathTraceAccumulator {
public:
    // Discard the accumulated image. The next trace overwrites rather than adds.
    void invalidate() {
        m_sampleCount = 0;
        m_resetPending = true;
    }

    // Compare this frame's camera against the one the accumulated samples were
    // traced with, and invalidate if the view moved. Returns true when the
    // image was discarded. Any camera change at all invalidates: a partially
    // moved accumulation is not a slower render, it is a wrong one.
    bool observeCamera(const CameraRenderData& camera) {
        const bool changed = !m_hasCamera ||
                             camera.view != m_camera.view ||
                             camera.proj != m_camera.proj;
        m_camera = camera;
        m_hasCamera = true;
        if (changed) {
            invalidate();
        }
        return changed;
    }

    // Compare this frame's scene against the one the accumulated samples were
    // traced against, and invalidate if it changed. `revision` is any value the
    // caller can cheaply derive that changes whenever the integrator's inputs
    // change — geometry added or removed, materials rewritten. Returns true
    // when the image was discarded.
    //
    // This is a coarse guard, not a complete one: an edit that changes a
    // material's colour without changing the revision keeps accumulating onto
    // the old image. That is what the panel's manual restart is for.
    bool observeScene(Uint64 revision) {
        const bool changed = !m_hasScene || revision != m_sceneRevision;
        m_sceneRevision = revision;
        m_hasScene = true;
        if (changed) {
            invalidate();
        }
        return changed;
    }

    // Samples the accumulator will hold after tracing `spp` more, capped so the
    // pass never overshoots maxSamples. 0 means there is nothing left to trace.
    Uint32 samplesToTrace(Uint32 spp, Uint32 maxSamples) const {
        if (m_sampleCount >= maxSamples) return 0;
        const Uint32 remaining = maxSamples - m_sampleCount;
        return spp < remaining ? spp : remaining;
    }

    // Called once a trace has actually been dispatched.
    void recordTraced(Uint32 samples) {
        m_sampleCount += samples;
        m_resetPending = false;
    }

    Uint32 sampleCount() const { return m_sampleCount; }
    // True while the next trace must overwrite the accumulator instead of
    // adding to it (nothing valid is in it yet).
    bool resetPending() const { return m_resetPending; }
    bool hasImage() const { return m_sampleCount > 0; }
    bool isConverged(Uint32 maxSamples) const { return m_sampleCount >= maxSamples; }

private:
    CameraRenderData m_camera{};
    bool m_hasCamera = false;
    Uint64 m_sceneRevision = 0;
    bool m_hasScene = false;
    Uint32 m_sampleCount = 0;
    bool m_resetPending = true;
};

class PathTracer {
public:
    // Compiles the integrator kernels. Safe to call on a backend without ray
    // tracing — it simply leaves the tracer unavailable.
    void init(RHI* rhi);
    void shutdown();

    // The accumulator is full-resolution and RGBA32F: rgb sums radiance, a
    // counts samples. 16-bit would lose the low bits of the sum long before a
    // long render finishes.
    void createTargets(Uint32 width, Uint32 height);
    void destroyTargets();

    // True when the kernels compiled and the accumulator exists.
    bool isAvailable() const { return m_accumulatePipeline.isValid() && m_accumTexture.isValid(); }

    // Trace another batch of samples into the accumulator. No-op when the view
    // cannot be traced or the render has already converged.
    void trace(const PathTraceSceneView& view);
    // Divide the accumulator by its sample count and write linear HDR into
    // `target`, which must be a storage-writable colour texture.
    void resolve(TextureHandle target);

    PathTraceAccumulator& accumulator() { return m_accumulator; }
    const PathTraceAccumulator& accumulator() const { return m_accumulator; }
    PathTraceSettings settings;

    Uint32 width() const { return m_width; }
    Uint32 height() const { return m_height; }

private:
    RHI* m_rhi = nullptr;

    ShaderHandle m_accumulateShader;
    ShaderHandle m_resolveShader;
    ComputePipelineHandle m_accumulatePipeline;
    ComputePipelineHandle m_resolvePipeline;

    TextureHandle m_accumTexture;
    Uint32 m_width = 0;
    Uint32 m_height = 0;

    PathTraceAccumulator m_accumulator;
};

} // namespace Vapor
