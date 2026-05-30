#pragma once
#include "camera.hpp"
#include "font_manager.hpp"
#include "graphics.hpp"
#include "scene.hpp"
#include <SDL3/SDL_video.h>
#include <entt/entt.hpp>
#include <functional>
#include <memory>
#include <vector>

// Forward declarations
namespace Rml {
    class Context;
}

namespace Vapor {
    class DebugDraw;
}

enum class GraphicsBackend { Metal, Vulkan };

enum class RenderPath { Forward, Deferred };

enum class BufferUsage { VERTEX, INDEX, UNIFORM, STORAGE, COPY_SRC, COPY_DST };

enum class RenderTargetUsage { COLOR_MSAA, COLOR, DEPTH_MSAA, DEPTH_STENCIL_MSAA, DEPTH, DEPTH_STENCIL };


struct GpuImageData {
    std::vector<uint8_t> data;
    uint32_t width;
    uint32_t height;
    uint32_t channelCount;
};

using ScreenshotCallback = std::function<void(const GpuImageData&)>;

class Renderer {
public:
    virtual ~Renderer() = default;
    virtual void init(SDL_Window* window) = 0;

    virtual void deinit() = 0;

    virtual void stage(std::shared_ptr<Scene> scene) = 0;

    virtual void draw(std::shared_ptr<Scene> scene, Camera& camera) = 0;
    virtual void draw(entt::registry& registry, std::shared_ptr<Scene> scene, Camera& camera) = 0;
    virtual void readPixelsAsync(ScreenshotCallback callback) = 0;

    virtual void setRenderPath(RenderPath path) = 0;
    virtual RenderPath getRenderPath() const = 0;

    // UI rendering (optional, implemented by backends that support it)
    // This method should set the RenderInterface and finalize RmlUI initialization
    virtual bool initUI() {
        return false; /* Default: not supported */
    }

    virtual std::shared_ptr<Vapor::DebugDraw> getDebugDraw() {
        return nullptr;
    }

    // Register a callback invoked each frame between ImGui::NewFrame() and
    // ImGui::Render(), allowing callers to draw custom ImGui windows without
    // touching the renderer internals.
    virtual void setImGuiCallback(std::function<void()> callback) {
        m_imGuiCallback = std::move(callback);
    }

    // ===== 2D/3D Batch Rendering API =====
    // Manual flush (for controlling draw order)
    virtual void flush2D() {
    }
    virtual void flush3D() {
    }

    // Quad drawing
    virtual void drawQuad2D(const glm::vec2& position, const glm::vec2& size, const glm::vec4& color) {
    }
    virtual void drawQuad2D(const glm::vec3& position, const glm::vec2& size, const glm::vec4& color) {
    }
    virtual void drawQuad2D(
        const glm::vec2& position,
        const glm::vec2& size,
        TextureHandle texture,
        const glm::vec4& tintColor = glm::vec4(1.0f)
    ) {
    }
    virtual void drawQuad2D(const glm::mat4& transform, const glm::vec4& color, int entityID = -1) {
    }
    virtual void drawQuad2D(
        const glm::mat4& transform,
        TextureHandle texture,
        const glm::vec2* texCoords,
        const glm::vec4& tintColor = glm::vec4(1.0f),
        int entityID = -1
    ) {
    }

    // 3D versions (world space with depth)
    virtual void drawQuad3D(const glm::vec3& position, const glm::vec2& size, const glm::vec4& color) {
    }
    virtual void drawQuad3D(
        const glm::vec3& position,
        const glm::vec2& size,
        TextureHandle texture,
        const glm::vec4& tintColor = glm::vec4(1.0f)
    ) {
    }
    virtual void drawQuad3D(const glm::mat4& transform, const glm::vec4& color, int entityID = -1) {
    }
    virtual void drawQuad3D(
        const glm::mat4& transform,
        TextureHandle texture,
        const glm::vec2* texCoords,
        const glm::vec4& tintColor = glm::vec4(1.0f),
        int entityID = -1
    ) {
    }

    // Rotated quad
    virtual void
        drawRotatedQuad2D(const glm::vec2& position, const glm::vec2& size, float rotation, const glm::vec4& color) {
    }
    virtual void drawRotatedQuad2D(
        const glm::vec2& position,
        const glm::vec2& size,
        float rotation,
        TextureHandle texture,
        const glm::vec4& tintColor = glm::vec4(1.0f)
    ) {
    }

    // Line drawing
    virtual void drawLine2D(const glm::vec2& p0, const glm::vec2& p1, const glm::vec4& color, float thickness = 1.0f) {
    }
    virtual void drawLine3D(const glm::vec3& p0, const glm::vec3& p1, const glm::vec4& color, float thickness = 1.0f) {
    }

    // Shape drawing
    virtual void
        drawRect2D(const glm::vec2& position, const glm::vec2& size, const glm::vec4& color, float thickness = 1.0f) {
    }
    virtual void drawCircle2D(const glm::vec2& center, float radius, const glm::vec4& color, int segments = 32) {
    }
    virtual void drawCircleFilled2D(const glm::vec2& center, float radius, const glm::vec4& color, int segments = 32) {
    }
    virtual void drawTriangle2D(const glm::vec2& p0, const glm::vec2& p1, const glm::vec2& p2, const glm::vec4& color) {
    }
    virtual void
        drawTriangleFilled2D(const glm::vec2& p0, const glm::vec2& p1, const glm::vec2& p2, const glm::vec4& color) {
    }

    // Batch statistics
    virtual Batch2DStats getBatch2DStats() const {
        return {};
    }
    virtual void resetBatch2DStats() {
    }

    // Texture creation for sprites
    virtual TextureHandle createTexture(const std::shared_ptr<Vapor::Image>& img) {
        return {};
    }

    // ===== Render-to-Texture API =====
    // Create a render texture that can be rendered to
    virtual RenderTextureHandle createRenderTexture(const RenderTextureDesc& desc) { return {}; }
    // Destroy a render texture
    virtual void destroyRenderTexture(RenderTextureHandle handle) {}
    // Get the texture handle for sampling (use as material texture or sprite)
    virtual TextureHandle getRenderTextureAsTexture(RenderTextureHandle handle) { return {}; }
    // Begin rendering to a render texture (with a callback that receives the scene and camera to render)
    // The callback should call draw commands that will be rendered to the texture
    virtual void renderToTexture(
        RenderTextureHandle target,
        std::shared_ptr<Scene> scene,
        Camera& camera,
        const glm::vec4& clearColor = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f)
    ) {}
    // Get render texture dimensions
    virtual glm::uvec2 getRenderTextureSize(RenderTextureHandle handle) { return glm::uvec2(0); }
    // Register a render texture with RmlUI (returns RmlUI texture handle)
    // This allows using the render texture as an image source in RmlUI documents
    virtual Uint64 registerRenderTextureForUI(RenderTextureHandle handle) { return 0; }

    // ===== Render Texture Post-Processing API =====
    // Apply bloom effect to render texture (requires HDR render texture)
    virtual void applyBloom(RenderTextureHandle target, float threshold = 1.0f, float strength = 0.5f) {}
    // Apply tone mapping to render texture (converts HDR to LDR)
    virtual void applyToneMapping(RenderTextureHandle target, float exposure = 1.0f) {}
    // Apply vignette effect
    virtual void applyVignette(RenderTextureHandle target, float strength = 0.3f, float radius = 0.8f) {}

    // ===== Particle System API =====
    // Called each frame by ParticleEmitterSystem before draw(); renderer uses
    // the first entry as the GPU attractor position for the next simulation step.
    virtual void setParticleAttractors(const std::vector<ParticleAttractorData>& attractors) {
        m_particleAttractors = attractors;
    }

    // Reserve a contiguous range of slots from the global GPU particle pool.
    // Returns the start index, or ~0u if capacity is exhausted.
    // Slots are permanent for the lifetime of the emitter.
    virtual uint32_t claimParticleSlots(uint32_t count) { return ~0u; }
    virtual void releaseParticleSlots(uint32_t slotBegin, uint32_t count) {}

    // Write CPU-computed initial particle state into the GPU buffer.
    // Must be called before draw() so the GPU sees fresh data this frame.
    virtual void uploadParticles(uint32_t slotBegin, const std::vector<GPUParticle>& particles) {}

    // ===== Font Rendering API =====
    // Load a font from file path with specified base size
    virtual FontHandle loadFont(const std::string& path, float baseSize) {
        return {};
    }
    // Unload a previously loaded font
    virtual void unloadFont(FontHandle handle) {
    }
    // Draw text at screen position (2D, no depth test)
    virtual void drawText2D(
        FontHandle font,
        const std::string& text,
        const glm::vec2& position,
        float scale = 1.0f,
        const glm::vec4& color = glm::vec4(1.0f)
    ) {
    }
    // Draw text at world position (3D, with depth test, billboard facing camera)
    virtual void drawText3D(
        FontHandle font,
        const std::string& text,
        const glm::vec3& worldPosition,
        float scale = 1.0f,
        const glm::vec4& color = glm::vec4(1.0f)
    ) {
    }
    // Measure text dimensions at given scale
    virtual glm::vec2 measureText(FontHandle font, const std::string& text, float scale = 1.0f) {
        return {};
    }
    // Get line height for a font at given scale
    virtual float getFontLineHeight(FontHandle font, float scale = 1.0f) {
        return 0.0f;
    }

protected:
    std::vector<ParticleAttractorData> m_particleAttractors;
    uint32_t m_particleSlotsAllocated = 0; // high-water mark for slot claims

    const Uint32 MAX_FRAMES_IN_FLIGHT = 3;
    const Uint32 MSAA_SAMPLE_COUNT = 4;
    const Uint32 MAX_INSTANCES = 5000;// Increased for large scenes like Bistro (2911 instances)
    const Uint32 MAX_DIRECTIONAL_LIGHTS = 4;
    const Uint32 MAX_POINT_LIGHTS = 1024;
    glm::vec4 clearColor = glm::vec4(0.0f, 0.5f, 1.0f, 1.0f);
    double clearDepth = 1.0;
    Uint32 clusterGridSizeX = 16;
    Uint32 clusterGridSizeY = 16;
    Uint32 clusterGridSizeZ = 24;
    Uint32 numClusters = clusterGridSizeX * clusterGridSizeY * clusterGridSizeZ;
    Uint32 currentFrameInFlight = 0;
    Uint32 frameNumber = 0;
    bool isInitialized = false;
    std::function<void()> m_imGuiCallback;

    int calculateMipmapLevelCount(Uint32 width, Uint32 height) const {
        return static_cast<int>(std::floor(std::log2(std::max(width, height))) + 1);
    }
};

#ifdef __APPLE__
std::unique_ptr<Renderer> createRendererMetal();
#endif
std::unique_ptr<Renderer> createRendererVulkan();

inline std::unique_ptr<Renderer> createRenderer(GraphicsBackend backend) {
    switch (backend) {
    case GraphicsBackend::Metal:
#ifdef __APPLE__
        return createRendererMetal();
#else
        return nullptr;
#endif
    case GraphicsBackend::Vulkan:
        return createRendererVulkan();
    default:
        return nullptr;
    }
}