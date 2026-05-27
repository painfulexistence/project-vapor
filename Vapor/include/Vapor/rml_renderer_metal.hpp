#pragma once
#ifdef __APPLE__

#include <Foundation/Foundation.hpp>
#include <Metal/Metal.hpp>
#include <RmlUi/Core.h>
#include <memory>
#include <unordered_map>

namespace Vapor {

// Shared RmlUI Metal renderer — implements Rml::RenderInterface.
// Both UIRenderer (CAPI) and Renderer_Metal (game) use this class.
// The caller manages the render pass; this class just draws to the provided encoder.
class RmlRendererMetal : public Rml::RenderInterface {
public:
    explicit RmlRendererMetal(MTL::Device* device);
    ~RmlRendererMetal() override;

    bool initialize();
    void shutdown();

    // Set the encoder before calling Rml::Context::Render().
    // logicalWidth/Height are UI coordinates (for ortho projection).
    // fbWidth/fbHeight are the actual texture dimensions (for HiDPI scaling).
    void setEncoder(MTL::RenderCommandEncoder* encoder, int logicalWidth, int logicalHeight,
                    int fbWidth, int fbHeight);
    void clearEncoder();

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

    // External texture support (for render-to-texture in RmlUI)
    Rml::TextureHandle registerExternalTexture(MTL::Texture* texture, int width, int height);
    void updateExternalTexture(Rml::TextureHandle handle, MTL::Texture* texture, int width, int height);

private:
    void createDefaultWhiteTexture();
    void createPipelineState();

    struct CompiledGeometry {
        NS::SharedPtr<MTL::Buffer> vertexBuffer;
        NS::SharedPtr<MTL::Buffer> indexBuffer;
        NS::UInteger indexCount = 0;
    };

    struct TextureData {
        NS::SharedPtr<MTL::Texture> texture;
        int width = 0, height = 0;
    };

    MTL::Device*               m_device  = nullptr;
    MTL::RenderCommandEncoder* m_encoder = nullptr;

    NS::SharedPtr<MTL::RenderPipelineState> m_pipelineState;
    NS::SharedPtr<MTL::DepthStencilState>   m_depthStencilState;
    NS::SharedPtr<MTL::Texture>             m_defaultWhiteTexture;

    std::unordered_map<Rml::CompiledGeometryHandle, CompiledGeometry> m_geometry;
    std::unordered_map<Rml::TextureHandle, TextureData>               m_textures;

    Rml::CompiledGeometryHandle m_nextGeomHandle = 1;
    Rml::TextureHandle          m_nextTexHandle  = 1;

    int   m_logicalWidth  = 0, m_logicalHeight = 0;
    float m_scaleX = 1.0f, m_scaleY = 1.0f;

    struct { bool enabled = false; int x = 0, y = 0, width = 0, height = 0; } m_scissor;
    Rml::Matrix4f m_transform;
};

} // namespace Vapor

#endif // __APPLE__
