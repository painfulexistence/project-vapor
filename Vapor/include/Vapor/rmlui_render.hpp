#pragma once
#include <RmlUi/Core/RenderInterface.h>
#include <glm/glm.hpp>
#include <SDL3/SDL_stdinc.h>
#include <vector>
#include <unordered_map>

namespace Vapor {

// Forward declaration - renderer will be set via setRenderer()
class RmlUiRenderer;

// RmlUi geometry handle
struct RmlUiGeometryHandle {
    Uint32 id;
    Uint32 vertexCount;
    Uint32 indexCount;
};

// RmlUi texture handle
struct RmlUiTextureHandle {
    Uint32 id;
    Uint32 width;
    Uint32 height;
};

// Vertex data for RmlUi
struct RmlUiVertex {
    glm::vec2 position;
    glm::u8vec4 color;
    glm::vec2 texCoord;
};

// Compiled geometry data
struct RmlUiCompiledGeometry {
    std::vector<RmlUiVertex> vertices;
    std::vector<Uint32> indices;
    Uint32 textureId; // 0 if no texture
};

// Render command for batching
struct RmlUiRenderCommand {
    Uint32 geometryId;
    glm::mat4 transform;
    glm::vec2 translation;
    Uint32 textureId;
    bool hasTexture;
    // Scissor region
    bool enableScissor;
    int scissorX, scissorY, scissorWidth, scissorHeight;
};

// Abstract renderer interface that Renderer_Metal will implement
class RmlUiRenderer {
public:
    virtual ~RmlUiRenderer() = default;

    // Called when RmlUi render interface is initialized
    virtual void rmluiInit() = 0;

    // Called when RmlUi render interface is shut down
    virtual void rmluiShutdown() = 0;

    // Create geometry (vertex + index buffers)
    virtual Uint32 rmluiCreateGeometry(const std::vector<RmlUiVertex>& vertices, const std::vector<Uint32>& indices) = 0;

    // Release geometry
    virtual void rmluiReleaseGeometry(Uint32 geometryId) = 0;

    // Create texture
    virtual Uint32 rmluiCreateTexture(Uint32 width, Uint32 height, const Uint8* data) = 0;

    // Release texture
    virtual void rmluiReleaseTexture(Uint32 textureId) = 0;

    // Set viewport dimensions
    virtual void rmluiSetViewport(int width, int height) = 0;

    // Begin rendering frame
    virtual void rmluiBeginFrame() = 0;

    // Render a geometry batch
    virtual void rmluiRenderGeometry(
        Uint32 geometryId,
        const glm::vec2& translation,
        Uint32 textureId,
        bool hasTexture
    ) = 0;

    // Enable scissor test
    virtual void rmluiEnableScissor(int x, int y, int width, int height) = 0;

    // Disable scissor test
    virtual void rmluiDisableScissor() = 0;

    // End rendering frame
    virtual void rmluiEndFrame() = 0;
};

// RmlUi Render Interface implementation
// This class only calls the renderer methods, actual GPU work is done in the renderer
class RmlUi_RenderInterface : public Rml::RenderInterface {
public:
    RmlUi_RenderInterface();
    ~RmlUi_RenderInterface() override;

    // Set the renderer backend
    void setRenderer(RmlUiRenderer* renderer);

    // Set viewport dimensions
    void setViewportDimensions(int width, int height);

    // Get viewport dimensions
    int getViewportWidth() const { return viewportWidth; }
    int getViewportHeight() const { return viewportHeight; }

    // Begin frame rendering
    void beginFrame();

    // End frame rendering
    void endFrame();

    // --- Rml::RenderInterface overrides ---

    // Compile geometry into GPU buffers
    Rml::CompiledGeometryHandle CompileGeometry(Rml::Span<const Rml::Vertex> vertices, Rml::Span<const int> indices) override;

    // Render compiled geometry
    void RenderGeometry(Rml::CompiledGeometryHandle geometry, Rml::Vector2f translation, Rml::TextureHandle texture) override;

    // Release compiled geometry
    void ReleaseGeometry(Rml::CompiledGeometryHandle geometry) override;

    // Load texture from file
    Rml::TextureHandle LoadTexture(Rml::Vector2i& texture_dimensions, const Rml::String& source) override;

    // Generate texture from pixel data
    Rml::TextureHandle GenerateTexture(Rml::Span<const Rml::byte> source, Rml::Vector2i source_dimensions) override;

    // Release texture
    void ReleaseTexture(Rml::TextureHandle texture) override;

    // Enable scissor region
    void EnableScissorRegion(bool enable) override;

    // Set scissor region
    void SetScissorRegion(Rml::Rectanglei region) override;

private:
    RmlUiRenderer* renderer = nullptr;
    int viewportWidth = 0;
    int viewportHeight = 0;

    // Scissor state
    bool scissorEnabled = false;
    int scissorX = 0, scissorY = 0;
    int scissorWidth = 0, scissorHeight = 0;

    // ID counters
    Uint32 nextGeometryId = 1;
    Uint32 nextTextureId = 1;

    // Geometry storage
    std::unordered_map<Uint32, RmlUiCompiledGeometry> compiledGeometries;
};

} // namespace Vapor
