#pragma once

#include <RmlUi/Core/RenderInterface.h>
#include <RmlUi/Core/Types.h>
// Metal header - include without private implementation in header
// Private implementation is defined in .cpp files that need it
#include <Metal/Metal.hpp>
#include <memory>
#include <unordered_map>

namespace Vapor {

    /**
     * RmlUI Render Interface implementation for Metal
     * Handles geometry compilation, rendering, and texture management
     */
    class RmlUiRenderer : public Rml::RenderInterface {
    public:
        RmlUiRenderer();
        ~RmlUiRenderer() override;

        // Initialize renderer with Metal device
        void Initialize(MTL::Device* device);

        // Shutdown and cleanup
        void Shutdown();

        // Frame management
        void BeginFrame(int width, int height, MTL::CommandBuffer* commandBuffer, MTL::Texture* renderTarget);
        void EndFrame();

        // Geometry compilation
        Rml::CompiledGeometryHandle
            CompileGeometry(Rml::Span<const Rml::Vertex> vertices, Rml::Span<const int> indices) override;

        void RenderGeometry(Rml::CompiledGeometryHandle geometry, Rml::Vector2f translation, Rml::TextureHandle texture)
            override;

        void ReleaseGeometry(Rml::CompiledGeometryHandle geometry) override;

        // Scissor region
        void EnableScissorRegion(bool enable) override;
        void SetScissorRegion(Rml::Rectanglei region) override;

        // Texture management
        Rml::TextureHandle LoadTexture(Rml::Vector2i& texture_dimensions, const Rml::String& source) override;
        Rml::TextureHandle GenerateTexture(Rml::Span<const Rml::byte> source, Rml::Vector2i source_dimensions) override;
        void ReleaseTexture(Rml::TextureHandle texture_handle) override;

        // Transform
        void SetTransform(const Rml::Matrix4f* transform) override;

        // Get current render encoder (for external use)
        MTL::RenderCommandEncoder* GetCurrentEncoder() const {
            return m_currentEncoder;
        }

    private:
        struct CompiledGeometry {
            NS::SharedPtr<MTL::Buffer> vertexBuffer;
            NS::SharedPtr<MTL::Buffer> indexBuffer;
            NS::UInteger indexCount;
        };

        struct TextureData {
            NS::SharedPtr<MTL::Texture> texture;
            int width;
            int height;
        };

        void CreateShaders();
        void CreatePipelineState();
        void CreateDefaultWhiteTexture();// Create default white texture for UI elements without texture

        MTL::Device* m_device = nullptr;
        MTL::CommandBuffer* m_currentCommandBuffer = nullptr;// Don't take ownership - managed by renderer
        MTL::RenderCommandEncoder* m_currentEncoder = nullptr;
        MTL::Texture* m_currentRenderTarget = nullptr;// Don't take ownership - autoreleased from drawable
        NS::SharedPtr<MTL::RenderPassDescriptor> m_currentPassDesc;// Keep pass descriptor alive until EndFrame()

        NS::SharedPtr<MTL::RenderPipelineState> m_pipelineState;
        NS::SharedPtr<MTL::DepthStencilState> m_depthStencilState;
        NS::SharedPtr<MTL::Texture> m_defaultWhiteTexture;// Default white texture for UI elements without texture

        std::unordered_map<Rml::CompiledGeometryHandle, CompiledGeometry> m_geometry;
        std::unordered_map<Rml::TextureHandle, TextureData> m_textures;

        Rml::CompiledGeometryHandle m_nextGeometryHandle = 1;
        Rml::TextureHandle m_nextTextureHandle = 1;

        int m_viewportWidth = 0;  // Logical (window) size for RmlUI coordinates
        int m_viewportHeight = 0;
        int m_framebufferWidth = 0;  // Actual framebuffer size for viewport/scissor
        int m_framebufferHeight = 0;

        struct ScissorRegion {
            bool enabled = false;
            int x = 0;
            int y = 0;
            int width = 0;
            int height = 0;
        } m_scissor;

        Rml::Matrix4f m_transform;
    };

}// namespace Vapor
