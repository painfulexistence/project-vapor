#pragma once
#include "camera.hpp"
#include "font_manager.hpp"
#include "graphics.hpp"
#include "scene.hpp"
#include <SDL3/SDL_video.h>
#include <memory>

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

// Render pass toggles for debugging/visualization
struct RenderPassToggles {
    bool prePass = true;
    bool tileCulling = true;
    bool mainPass = true;
    bool particles = true;
    bool postProcess = true;
};

class Renderer {
public:
    virtual ~Renderer() = default;
    virtual void init(SDL_Window* window) = 0;

    virtual void deinit() = 0;

    virtual void stage(std::shared_ptr<Scene> scene) = 0;

    virtual void draw(std::shared_ptr<Scene> scene, Camera& camera) = 0;

    virtual void setRenderPath(RenderPath path) = 0;
    virtual RenderPath getRenderPath() const = 0;

    // Render pass toggles for debugging
    RenderPassToggles& getPassToggles() { return passToggles; }
    const RenderPassToggles& getPassToggles() const { return passToggles; }

    // UI rendering (optional, implemented by backends that support it)
    // This method should set the RenderInterface and finalize RmlUI initialization
    virtual bool initUI() {
        return false; /* Default: not supported */
    }

    virtual std::shared_ptr<Vapor::DebugDraw> getDebugDraw() {
        return nullptr;
    }

    // ===== 2D/3D Batch Rendering API =====
    // Manual flush (for controlling draw order)
    virtual void flush2D() {}
    virtual void flush3D() {}

    // Quad drawing
    virtual void drawQuad2D(const glm::vec2& position, const glm::vec2& size, const glm::vec4& color) {}
    virtual void drawQuad2D(const glm::vec3& position, const glm::vec2& size, const glm::vec4& color) {}
    virtual void drawQuad2D(const glm::vec2& position, const glm::vec2& size, TextureHandle texture, const glm::vec4& tintColor = glm::vec4(1.0f)) {}
    virtual void drawQuad2D(const glm::mat4& transform, const glm::vec4& color, int entityID = -1) {}
    virtual void drawQuad2D(const glm::mat4& transform, TextureHandle texture, const glm::vec2* texCoords, const glm::vec4& tintColor = glm::vec4(1.0f), int entityID = -1) {}

    // 3D versions (world space with depth)
    virtual void drawQuad3D(const glm::vec3& position, const glm::vec2& size, const glm::vec4& color) {}
    virtual void drawQuad3D(const glm::vec3& position, const glm::vec2& size, TextureHandle texture, const glm::vec4& tintColor = glm::vec4(1.0f)) {}
    virtual void drawQuad3D(const glm::mat4& transform, const glm::vec4& color, int entityID = -1) {}
    virtual void drawQuad3D(const glm::mat4& transform, TextureHandle texture, const glm::vec2* texCoords, const glm::vec4& tintColor = glm::vec4(1.0f), int entityID = -1) {}

    // Rotated quad
    virtual void drawRotatedQuad2D(const glm::vec2& position, const glm::vec2& size, float rotation, const glm::vec4& color) {}
    virtual void drawRotatedQuad2D(const glm::vec2& position, const glm::vec2& size, float rotation, TextureHandle texture, const glm::vec4& tintColor = glm::vec4(1.0f)) {}

    // Line drawing
    virtual void drawLine2D(const glm::vec2& p0, const glm::vec2& p1, const glm::vec4& color, float thickness = 1.0f) {}
    virtual void drawLine3D(const glm::vec3& p0, const glm::vec3& p1, const glm::vec4& color, float thickness = 1.0f) {}

    // Shape drawing
    virtual void drawRect2D(const glm::vec2& position, const glm::vec2& size, const glm::vec4& color, float thickness = 1.0f) {}
    virtual void drawCircle2D(const glm::vec2& center, float radius, const glm::vec4& color, int segments = 32) {}
    virtual void drawCircleFilled2D(const glm::vec2& center, float radius, const glm::vec4& color, int segments = 32) {}
    virtual void drawTriangle2D(const glm::vec2& p0, const glm::vec2& p1, const glm::vec2& p2, const glm::vec4& color) {}
    virtual void drawTriangleFilled2D(const glm::vec2& p0, const glm::vec2& p1, const glm::vec2& p2, const glm::vec4& color) {}

    // Batch statistics
    virtual Batch2DStats getBatch2DStats() const { return {}; }
    virtual void resetBatch2DStats() {}

    // Texture creation for sprites
    virtual TextureHandle createTexture(const std::shared_ptr<Image>& img) { return {}; }

    // ===== Font Rendering API =====
    // Load a font from file path with specified base size
    virtual FontHandle loadFont(const std::string& path, float baseSize) { return {}; }
    // Unload a previously loaded font
    virtual void unloadFont(FontHandle handle) {}
    // Draw text at screen position (2D, no depth test)
    virtual void drawText2D(
        FontHandle font,
        const std::string& text,
        const glm::vec2& position,
        float scale = 1.0f,
        const glm::vec4& color = glm::vec4(1.0f)
    ) {}
    // Draw text at world position (3D, with depth test, billboard facing camera)
    virtual void drawText3D(
        FontHandle font,
        const std::string& text,
        const glm::vec3& worldPosition,
        float scale = 1.0f,
        const glm::vec4& color = glm::vec4(1.0f)
    ) {}
    // Measure text dimensions at given scale
    virtual glm::vec2 measureText(FontHandle font, const std::string& text, float scale = 1.0f) { return {}; }
    // Get line height for a font at given scale
    virtual float getFontLineHeight(FontHandle font, float scale = 1.0f) { return 0.0f; }

protected:
    RenderPassToggles passToggles;
    const Uint32 MAX_FRAMES_IN_FLIGHT = 3;
    const Uint32 MSAA_SAMPLE_COUNT = 4;
    const Uint32 MAX_INSTANCES = 5000;// Increased for large scenes like Bistro (2911 instances)
    glm::vec4 clearColor = glm::vec4(0.0f, 0.5f, 1.0f, 1.0f);
    double clearDepth = 1.0;
    Uint32 clusterGridSizeX = 16;
    Uint32 clusterGridSizeY = 16;
    Uint32 clusterGridSizeZ = 24;
    Uint32 numClusters = clusterGridSizeX * clusterGridSizeY * clusterGridSizeZ;
    Uint32 currentFrameInFlight = 0;
    Uint32 frameNumber = 0;
    bool isInitialized = false;

    int calculateMipmapLevelCount(Uint32 width, Uint32 height) const {
        return static_cast<int>(std::floor(std::log2(std::max(width, height))) + 1);
    }
};

std::unique_ptr<Renderer> createRendererMetal();
std::unique_ptr<Renderer> createRendererVulkan();

inline std::unique_ptr<Renderer> createRenderer(GraphicsBackend backend) {
    switch (backend) {
    case GraphicsBackend::Metal:
        return createRendererMetal();
    case GraphicsBackend::Vulkan:
        return createRendererVulkan();
    default:
        return nullptr;
    }
}