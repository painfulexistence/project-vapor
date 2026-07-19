#pragma once
// ============================================================================
// IRenderer — abstract renderer interface shared by every backend renderer.
//
// Two concrete implementations exist:
//   * Renderer         (src/renderer.cpp)        — the RHI renderer; used by
//                                                   the Vulkan backend (and
//                                                   Metal-via-RHI if selected).
//   * Renderer_Metal   (src/renderer_metal.cpp)  — the full-feature native
//                                                   Metal renderer (45 passes,
//                                                   RT/GIBS/water/clouds/…).
//
// createRenderer() picks the implementation per backend and returns a
// unique_ptr<IRenderer>, so gameplay code (main.cpp), the engine core, the
// video recorder, etc. all drive the renderer through this interface without
// knowing which backend is active.
//
// The frame model is the RHI renderer's: beginFrame() → (ImGui::NewFrame in
// caller) → invokeImGuiCallback() → draw* / draw() → (ImGui::Render in caller)
// → endFrame(). Native backends adapt their internal frame loop to match.
//
// Methods have default (mostly no-op) implementations so each backend only
// overrides what it actually supports.
// ============================================================================

#include "rhi.hpp"            // TextureHandle, PixelFormat
#include "render_data.hpp"    // CameraRenderData
#include "camera.hpp"
#include "graphics.hpp"       // Image, FontHandle via font_manager
#include "font_manager.hpp"   // FontHandle
#include "render_scene.hpp"
#include <SDL3/SDL_video.h>
#include <entt/entt.hpp>
#include <functional>
#include <memory>
#include <vector>
#include <string>

namespace Vapor {
// Directional shadow map resolution — a renderer-level decision shared by every
// backend so the Metal and Vulkan cascade implementations stay in lockstep
// (previously hardcoded separately: Metal 4096, Vulkan 2048). Raising/lowering
// this once here changes both backends. Cascade array VRAM = 3 * size^2 * 4B.
inline constexpr uint32_t kDirectionalShadowMapSize = 4096;  // PSSM cascade array
inline constexpr uint32_t kNearShadowMapSize        = 4096;  // independent near-field map
}

namespace Rml {
    class Context;
}
namespace Vapor {

class DebugDraw;
class VoxelWorld;

// One raymarched micro-voxel volume, resolved from the ECS
// VoxelVolumeComponent by VoxelVolumeSystem and pushed per frame. The
// renderer keys its GPU mirrors (page table / brick pool buffers) on the
// VoxelWorld pointer and pulls per-brick dirty batches from it.
struct VoxelVolumeDraw {
    std::shared_ptr<VoxelWorld> world;
    glm::vec3 origin = glm::vec3(0.0f);  // world-space min corner
};

// Graphics backend selection
enum class GraphicsBackend {
    Metal,
    Vulkan
};

// Render path selection
enum class RenderPath {
    Forward,    // Simple forward rendering
    Deferred,   // Deferred rendering
    Clustered   // Clustered forward/deferred with tiled light culling
};

// Screenshot callback
struct GpuImageData {
    std::vector<uint8_t> data;
    uint32_t width;
    uint32_t height;
    uint32_t channelCount;
};

using ScreenshotCallback = std::function<void(const GpuImageData&)>;

// Batch rendering stats. NOTE: graphics_batch2d.hpp defines a richer global
// Batch2DStats but cannot be included here (it redefines BlendMode, which is
// also in graphics.hpp). getBatch2DStats() is not part of IRenderer (never
// called polymorphically); each renderer exposes its own stats type.

// Render texture descriptor
struct RenderTextureDesc {
    uint32_t width = 1920;
    uint32_t height = 1080;
    PixelFormat format = PixelFormat::RGBA8_UNORM;
    bool isHDR = false;
    bool hasDepth = true;
    uint32_t sampleCount = 1;
};

// Render texture handle
struct RenderTextureHandle {
    uint32_t id = UINT32_MAX;
    bool isValid() const { return id != UINT32_MAX; }
};

// ============================================================================
// IRenderer
// ============================================================================
class IRenderer {
public:
    virtual ~IRenderer() = default;

    // ---- Lifecycle -------------------------------------------------------
    virtual void shutdown() {}

    // ---- Scene / staging -------------------------------------------------
    virtual void stage(std::shared_ptr<RenderScene> scene) {}

    // ---- Frame ----------------------------------------------------------
    // beginFrame(): acquire the drawable/command buffer and run the backend
    //   ImGui NewFrame. Call ImGui::NewFrame() in caller code afterwards.
    virtual void beginFrame(const CameraRenderData& camera) {}
    // invokeImGuiCallback(): draw the built-in engine debug panel and the
    //   registered app/engine callbacks. Call after ImGui::NewFrame().
    virtual void invokeImGuiCallback() {}
    // draw(): collect drawables from the scene/registry and run the passes.
    virtual void draw(std::shared_ptr<RenderScene> scene, Camera& camera) {}
    virtual void draw(entt::registry& registry, std::shared_ptr<RenderScene> scene, Camera& camera) {}
    // endFrame(): render ImGui draw data (after ImGui::Render()) and present.
    virtual void endFrame() {}

    // C-API convenience: one-call frame + resize (native renderers may impl).
    virtual void renderFrame() {}
    virtual void resize(uint32_t width, uint32_t height) {}

    // ---- Screenshots -----------------------------------------------------
    virtual void readPixelsAsync(ScreenshotCallback callback) {}

    // ---- Render path -----------------------------------------------------
    virtual void setRenderPath(RenderPath path) {}
    virtual RenderPath getRenderPath() const { return RenderPath::Forward; }

    // ---- UI --------------------------------------------------------------
    virtual bool initUI() { return false; }
    virtual std::shared_ptr<Vapor::DebugDraw> getDebugDraw() { return nullptr; }

    virtual void setImGuiCallback(std::function<void()> callback) {
        m_imGuiCallback = std::move(callback);
    }
    virtual void setEngineWindowCallback(std::function<void()> callback) {
        m_engineWindowCallback = std::move(callback);
    }
    virtual void setImGuiFrameCallback(std::function<void()> callback) {
        m_imGuiFrameCallback = std::move(callback);
    }
    bool isImGuiVisible() const { return m_imGuiVisible; }
    void setImGuiVisible(bool visible) { m_imGuiVisible = visible; }

    virtual void uploadRectLightVideoTexture(const uint8_t* /*rgba*/, uint32_t /*width*/, uint32_t /*height*/) {}

    // ---- 2D/3D batch drawing --------------------------------------------
    virtual void flush2D() {}
    virtual void flush3D() {}

    virtual void drawQuad2D(const glm::vec2& position, const glm::vec2& size, const glm::vec4& color) {}
    virtual void drawQuad2D(const glm::vec3& position, const glm::vec2& size, const glm::vec4& color) {}
    virtual void drawQuad2D(const glm::vec2& position, const glm::vec2& size, TextureHandle texture,
                            const glm::vec4& tintColor = glm::vec4(1.0f)) {}
    virtual void drawQuad2D(const glm::mat4& transform, const glm::vec4& color, int entityID = -1) {}
    virtual void drawQuad2D(const glm::mat4& transform, TextureHandle texture, const glm::vec2* texCoords,
                            const glm::vec4& tintColor = glm::vec4(1.0f), int entityID = -1) {}

    virtual void drawQuad3D(const glm::vec3& position, const glm::vec2& size, const glm::vec4& color) {}
    virtual void drawQuad3D(const glm::vec3& position, const glm::vec2& size, TextureHandle texture,
                            const glm::vec4& tintColor = glm::vec4(1.0f)) {}
    virtual void drawQuad3D(const glm::mat4& transform, const glm::vec4& color, int entityID = -1) {}
    virtual void drawQuad3D(const glm::mat4& transform, TextureHandle texture, const glm::vec2* texCoords,
                            const glm::vec4& tintColor = glm::vec4(1.0f), int entityID = -1) {}

    virtual void drawRotatedQuad2D(const glm::vec2& position, const glm::vec2& size, float rotation,
                                   const glm::vec4& color) {}
    virtual void drawRotatedQuad2D(const glm::vec2& position, const glm::vec2& size, float rotation,
                                   TextureHandle texture, const glm::vec4& tintColor = glm::vec4(1.0f)) {}

    virtual void drawLine2D(const glm::vec2& p0, const glm::vec2& p1, const glm::vec4& color, float thickness = 1.0f) {}
    virtual void drawLine3D(const glm::vec3& p0, const glm::vec3& p1, const glm::vec4& color, float thickness = 1.0f) {}

    virtual void drawRect2D(const glm::vec2& position, const glm::vec2& size, const glm::vec4& color, float thickness = 1.0f) {}
    virtual void drawCircle2D(const glm::vec2& center, float radius, const glm::vec4& color, int segments = 32) {}
    virtual void drawCircleFilled2D(const glm::vec2& center, float radius, const glm::vec4& color, int segments = 32) {}
    virtual void drawTriangle2D(const glm::vec2& p0, const glm::vec2& p1, const glm::vec2& p2, const glm::vec4& color) {}
    virtual void drawTriangleFilled2D(const glm::vec2& p0, const glm::vec2& p1, const glm::vec2& p2, const glm::vec4& color) {}

    // getBatch2DStats()/resetBatch2DStats() are intentionally NOT here — the
    // stats type differs per renderer and is never queried polymorphically.

    // ---- Environment / IBL ----------------------------------------------
    // Load an equirectangular HDR image as the IBL environment source. No-op on
    // renderers/backends without an IBL cubemap chain.
    virtual void loadHDRI(const std::string& path) {}

    // ---- Fonts / text ----------------------------------------------------
    virtual FontHandle loadFont(const std::string& path, float baseSize) { return {}; }
    virtual void unloadFont(FontHandle handle) {}
    virtual void drawText2D(FontHandle font, const std::string& text, const glm::vec2& position,
                            float scale = 1.0f, const glm::vec4& color = glm::vec4(1.0f)) {}
    virtual void drawText3D(FontHandle font, const std::string& text, const glm::vec3& worldPosition,
                            float scale = 1.0f, const glm::vec4& color = glm::vec4(1.0f)) {}
    virtual glm::vec2 measureText(FontHandle font, const std::string& text, float scale = 1.0f) { return {}; }
    virtual float getFontLineHeight(FontHandle font, float scale = 1.0f) { return 0.0f; }

    // ---- Texture creation ------------------------------------------------
    virtual TextureHandle createTexture(const std::shared_ptr<Vapor::Image>& img) { return {}; }
    virtual void updateTexture(TextureHandle handle, const std::shared_ptr<Vapor::Image>& img) {}

    // ---- Render-to-texture ----------------------------------------------
    virtual RenderTextureHandle createRenderTexture(const RenderTextureDesc& desc) { return {}; }
    virtual void destroyRenderTexture(RenderTextureHandle handle) {}
    virtual TextureHandle getRenderTextureAsTexture(RenderTextureHandle handle) { return {}; }
    virtual void renderToTexture(RenderTextureHandle target, std::shared_ptr<RenderScene> scene, Camera& camera,
                                 const glm::vec4& clearColor = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f)) {}
    virtual glm::uvec2 getRenderTextureSize(RenderTextureHandle handle) { return glm::uvec2(0); }
    virtual Uint64 registerRenderTextureForUI(RenderTextureHandle handle) { return 0; }

    // ---- Render-texture post-processing ----------------------------------
    virtual void applyBloom(RenderTextureHandle target, float threshold = 1.0f, float strength = 0.5f) {}
    virtual void applyToneMapping(RenderTextureHandle target, float exposure = 1.0f) {}
    virtual void applyVignette(RenderTextureHandle target, float strength = 0.3f, float radius = 0.8f) {}

    // ---- ECS particle integration ----------------------------------------
    // Claim/release a contiguous range of slots in the shared GPU particle pool.
    // Returns ~0u if the pool is full.
    virtual uint32_t claimParticleSlots(uint32_t count) { return ~0u; }
    virtual void releaseParticleSlots(uint32_t slotBegin, uint32_t count) {}
    // Upload initial particle state into previously claimed slots.
    virtual void uploadParticles(uint32_t slotBegin,
                                 const std::vector<GPUParticleData>& particles) {}
    // Per-frame particle force field gathered by ParticleForceFieldSystem.
    virtual void setParticleForceField(const ParticleForceField& field) {}
    // Freeze the GPU sim (deltaTime=0); particles stay in place but are still rendered.
    virtual void setParticleSimPaused(bool paused) {}
    // Hide toggle — gate the particle render only; the sim keeps running.
    virtual void setParticleVisible(bool visible) {}
    // Per-frame particle draw list gathered by ParticleRenderSystem — one packet
    // per emitter (per-material draws: blend mode + texture per packet). Backends
    // without per-material particle draws (legacy Metal) ignore this.
    virtual void setParticleDrawList(const std::vector<ParticleDrawPacket>& draws) {}

    // Sky/atmosphere description resolved from the ECS SkyComponent by SkySystem.
    // Pushed only when the component changes. The sun is not included here — it
    // is light-driven (see LightGatherSystem).
    virtual void setSky(const SkyRenderData& sky) {}

    // Shared wind resolved from the ECS WindFieldComponent by WindSystem. Sets
    // the wind DIRECTION consumed by the cloud (and Metal fog) passes; per-medium
    // scroll speed stays local to each effect. Pushed only when a WindFieldComponent
    // exists — otherwise the backends keep their panel-set wind.
    virtual void setWind(const WindRenderData& wind) {}

    // Request a rebake of the environment IBL (sky capture -> irradiance /
    // prefilter). SkySystem calls this when the sun has moved far enough to
    // restale the captured environment. Both backends satisfy it identically by
    // setting their iblNeedsUpdate flag, so the throttle decision lives in one
    // place (the ECS layer), not duplicated per backend.
    virtual void requestIBLUpdate() {}

    // Per-frame micro-voxel volume list resolved from the ECS
    // VoxelVolumeComponent by VoxelVolumeSystem. The RHI renderer raymarches
    // them (MicroVoxel pass); backends without the pass ignore the list.
    virtual void setVoxelVolumes(const std::vector<Vapor::VoxelVolumeDraw>& volumes) {}

    // In-place geometry rewrite for a registered mesh — the streaming hook
    // (TerrainSystem's tile slot pools): counts must match registration.
    // Default false = backend can't stream geometry; callers keep the
    // registered contents (the terrain then stays at its boot LOD).
    virtual bool updateMeshGeometry(MeshId /*id*/, const std::vector<Vapor::VertexData>& /*vertices*/,
                                    const std::vector<Uint32>& /*indices*/) { return false; }

protected:
    std::function<void()> m_imGuiCallback;
    std::function<void()> m_engineWindowCallback;
    std::function<void()> m_imGuiFrameCallback;
    bool m_imGuiVisible = true;
};

// ============================================================================
// Factory — creates the renderer for the requested backend. The RHI (if any)
// is created internally and owned by the renderer.
// ============================================================================
std::unique_ptr<IRenderer> createRenderer(GraphicsBackend backend, SDL_Window* window);

#ifdef __APPLE__
// Native Metal renderer factory (defined in src/renderer_metal.cpp). Creates
// and initializes the full-feature Metal renderer for the given window.
std::unique_ptr<IRenderer> createRendererMetal(SDL_Window* window);
#endif

} // namespace Vapor

// Transitional shim: these types lived at global scope before the namespace
// unification; unqualified call sites keep compiling while they migrate to
// Vapor:: qualification. Remove once call sites are migrated.
using namespace Vapor;
