#pragma once

#include "rhi.hpp"
#include "irenderer.hpp"  // GraphicsBackend

#include <RmlUi/Core.h>
#include <glm/glm.hpp>
#include <unordered_map>

namespace Vapor {

// Cross-backend RmlUI renderer — implements Rml::RenderInterface on top of the
// RHI, so RmlUI works on any backend the RHI supports (Vulkan today, Metal via
// the RHI bring-up path). The counterpart of RmlRendererMetal, minus the
// native-Metal coupling. The caller opens the render pass (swapchain, load)
// and brackets Rml::Context::Render() with beginFrame()/endFrame().
class RmlRendererRHI : public Rml::RenderInterface {
public:
    RmlRendererRHI(RHI* rhi, GraphicsBackend backend);
    ~RmlRendererRHI() override;

    bool initialize();
    void shutdown();

    // Call inside an open render pass, before Rml::Context::Render().
    // logicalWidth/Height are UI coordinates (ortho projection); fbWidth/
    // fbHeight are framebuffer pixels (HiDPI scaling + scissor space).
    void beginFrame(int logicalWidth, int logicalHeight, int fbWidth, int fbHeight);
    void endFrame();

    // Rml::RenderInterface
    Rml::CompiledGeometryHandle CompileGeometry(Rml::Span<const Rml::Vertex> vertices,
                                                Rml::Span<const int> indices) override;
    void RenderGeometry(Rml::CompiledGeometryHandle geometry, Rml::Vector2f translation,
                        Rml::TextureHandle texture) override;
    void ReleaseGeometry(Rml::CompiledGeometryHandle geometry) override;

    void EnableScissorRegion(bool enable) override;
    void SetScissorRegion(Rml::Rectanglei region) override;

    Rml::TextureHandle LoadTexture(Rml::Vector2i& dimensions, const Rml::String& source) override;
    Rml::TextureHandle GenerateTexture(Rml::Span<const Rml::byte> source,
                                       Rml::Vector2i dimensions) override;
    void ReleaseTexture(Rml::TextureHandle texture) override;

    void SetTransform(const Rml::Matrix4f* transform) override;

private:
    void applyScissor();

    RHI* m_rhi = nullptr;
    GraphicsBackend m_backend = GraphicsBackend::Vulkan;

    PipelineHandle m_pipeline;
    ShaderHandle m_vertexShader;
    ShaderHandle m_fragmentShader;
    SamplerHandle m_sampler;
    TextureHandle m_whiteTexture;

    struct CompiledGeometry {
        BufferHandle vertexBuffer;
        BufferHandle indexBuffer;
        Uint32 indexCount = 0;
    };
    struct TextureData {
        TextureHandle texture;
        int width = 0;
        int height = 0;
    };
    std::unordered_map<Rml::CompiledGeometryHandle, CompiledGeometry> m_geometry;
    std::unordered_map<Rml::TextureHandle, TextureData> m_textures;
    Rml::CompiledGeometryHandle m_nextGeomHandle = 1;
    Rml::TextureHandle m_nextTexHandle = 1;

    Rml::Matrix4f m_transform;
    glm::mat4 m_projection = glm::mat4(1.0f);
    bool m_inFrame = false;
    int m_logicalWidth = 0, m_logicalHeight = 0;
    int m_fbWidth = 0, m_fbHeight = 0;
    float m_scaleX = 1.0f, m_scaleY = 1.0f;
    struct { bool enabled = false; int x = 0, y = 0, width = 0, height = 0; } m_scissor;
};

} // namespace Vapor
