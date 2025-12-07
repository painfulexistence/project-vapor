#include "rmlui_renderer.hpp"
#include "helper.hpp"
#include <RmlUi/Core.h>
#include <RmlUi/Core/RenderInterface.h>
#include <fmt/core.h>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
// #include <spdlog/spdlog.h>

namespace Vapor {

    RmlUiRenderer::RmlUiRenderer() {
        m_transform = Rml::Matrix4f::Identity();
    }

    RmlUiRenderer::~RmlUiRenderer() {
        Shutdown();
    }

    void RmlUiRenderer::Initialize(MTL::Device* device) {
        if (!device) {
            // spdlog::error("RmlUiRenderer::Initialize: Metal device is null");
            return;
        }

        m_device = device;
        CreateDefaultWhiteTexture();
        CreatePipelineState();
        // spdlog::info("RmlUi renderer initialized");
    }

    void RmlUiRenderer::Shutdown() {
        m_geometry.clear();
        m_textures.clear();
        m_pipelineState.reset();
        m_depthStencilState.reset();
        m_defaultWhiteTexture.reset();
        m_device = nullptr;
    }

    void RmlUiRenderer::BeginFrame(
        int width, int height, MTL::CommandBuffer* commandBuffer, MTL::Texture* renderTarget
    ) {
        if (!commandBuffer || !renderTarget) {
            // spdlog::error("RmlUiRenderer::BeginFrame: Invalid command buffer or render target");
            return;
        }

        // width/height are logical (window) size for RmlUI coordinate system
        m_logicalWidth = width;
        m_logicalHeight = height;

        // Get framebuffer size and calculate HiDPI scale
        int fbWidth = static_cast<int>(renderTarget->width());
        int fbHeight = static_cast<int>(renderTarget->height());
        m_scaleX = width > 0 ? static_cast<float>(fbWidth) / width : 1.0f;
        m_scaleY = height > 0 ? static_cast<float>(fbHeight) / height : 1.0f;

        // Don't take ownership of commandBuffer - it's managed by the renderer
        // Just store raw pointer for this frame
        m_currentCommandBuffer = commandBuffer;
        // Don't take ownership of renderTarget - it's from drawable->texture() which is autoreleased
        // Just store raw pointer for this frame
        m_currentRenderTarget = renderTarget;

        // Create render pass descriptor (autoreleased, don't take ownership)
        // Note: Blending is configured in RenderPipelineDescriptor, not RenderPassDescriptor
        // Store passDesc as member to keep it alive until EndFrame()
        m_currentPassDesc = NS::TransferPtr(MTL::RenderPassDescriptor::renderPassDescriptor());
        auto colorAttachment = m_currentPassDesc->colorAttachments()->object(0);
        colorAttachment->setTexture(renderTarget);
        colorAttachment->setLoadAction(MTL::LoadActionLoad);// Load existing content (scene)
        colorAttachment->setStoreAction(MTL::StoreActionStore);

        // No depth attachment for UI - UI should render on top without depth testing

        m_currentEncoder = commandBuffer->renderCommandEncoder(m_currentPassDesc.get());

        // Set viewport to match render target (framebuffer) size
        MTL::Viewport viewport;
        viewport.originX = 0.0;
        viewport.originY = 0.0;
        viewport.width = static_cast<double>(fbWidth);
        viewport.height = static_cast<double>(fbHeight);
        viewport.znear = 0.0;
        viewport.zfar = 1.0;
        m_currentEncoder->setViewport(viewport);

        // Set scissor rect to full framebuffer (disable scissor by default)
        MTL::ScissorRect scissorRect;
        scissorRect.x = 0;
        scissorRect.y = 0;
        scissorRect.width = static_cast<NS::UInteger>(fbWidth);
        scissorRect.height = static_cast<NS::UInteger>(fbHeight);
        m_currentEncoder->setScissorRect(scissorRect);
    }

    void RmlUiRenderer::EndFrame() {
        if (m_currentEncoder) {
            m_currentEncoder->endEncoding();
            m_currentEncoder = nullptr;
        }
        // Don't reset commandBuffer - it's not owned by us
        m_currentCommandBuffer = nullptr;
        // Don't reset renderTarget - it's not owned by us (autoreleased)
        m_currentRenderTarget = nullptr;
        // Release pass descriptor
        m_currentPassDesc.reset();
    }

    Rml::CompiledGeometryHandle
        RmlUiRenderer::CompileGeometry(Rml::Span<const Rml::Vertex> vertices, Rml::Span<const int> indices) {
        if (!m_device) {
            // spdlog::error("RmlUiRenderer::CompileGeometry: Device not initialized");
            return 0;
        }

        // Create vertex buffer
        auto vertexBuffer =
            NS::TransferPtr(m_device->newBuffer(vertices.size() * sizeof(Rml::Vertex), MTL::ResourceStorageModeShared));
        memcpy(vertexBuffer->contents(), vertices.data(), vertices.size() * sizeof(Rml::Vertex));

        // Create index buffer
        auto indexBuffer =
            NS::TransferPtr(m_device->newBuffer(indices.size() * sizeof(int), MTL::ResourceStorageModeShared));
        memcpy(indexBuffer->contents(), indices.data(), indices.size() * sizeof(int));

        CompiledGeometry geom;
        geom.vertexBuffer = vertexBuffer;
        geom.indexBuffer = indexBuffer;
        geom.indexCount = static_cast<NS::UInteger>(indices.size());

        Rml::CompiledGeometryHandle handle = m_nextGeometryHandle++;
        m_geometry[handle] = std::move(geom);

        return handle;
    }

    void RmlUiRenderer::RenderGeometry(
        Rml::CompiledGeometryHandle geometry, Rml::Vector2f translation, Rml::TextureHandle texture
    ) {
        if (!m_currentEncoder) {
            fmt::print("RmlUiRenderer::RenderGeometry: No active render encoder\n");
            return;
        }

        auto it = m_geometry.find(geometry);
        if (it == m_geometry.end()) {
            fmt::print("RmlUiRenderer::RenderGeometry: Geometry handle {} not found\n", geometry);
            return;
        }

        const CompiledGeometry& geom = it->second;
        fmt::print(
            "RmlUiRenderer::RenderGeometry: Drawing geometry {} with {} indices, translation=({}, {}), viewport=({}, "
            "{})\n",
            geometry,
            geom.indexCount,
            translation.x,
            translation.y,
            m_logicalWidth,
            m_logicalHeight
        );

        // Set pipeline state
        if (!m_pipelineState) {
            fmt::print("RmlUiRenderer::RenderGeometry: Pipeline state is null!\n");
            return;
        }
        m_currentEncoder->setRenderPipelineState(m_pipelineState.get());
        m_currentEncoder->setDepthStencilState(m_depthStencilState.get());
        // Disable culling for UI (UI elements can be rendered from any angle)
        m_currentEncoder->setCullMode(MTL::CullModeNone);

        // Calculate projection matrix (orthographic, top-left origin)
        // RmlUI uses top-left origin (0,0 at top-left), Metal uses bottom-left origin for NDC
        // So we need to flip Y: ortho(left, right, bottom, top) -> ortho(0, width, height, 0)
        glm::mat4 projection = glm::ortho(0.0f, (float)m_logicalWidth, (float)m_logicalHeight, 0.0f, -1.0f, 1.0f);
        fmt::print(
            "RmlUiRenderer::RenderGeometry: Projection matrix: width={}, height={}\n", m_logicalWidth, m_logicalHeight
        );

        // Apply translation to transform
        glm::mat4 transform = glm::make_mat4(m_transform.data());
        transform = glm::translate(transform, glm::vec3(translation.x, translation.y, 0.0f));

        // Create uniform buffer
        struct Uniforms {
            glm::mat4 projectionMatrix;
            glm::mat4 transformMatrix;
        } uniforms;
        uniforms.projectionMatrix = projection;
        uniforms.transformMatrix = transform;

        m_currentEncoder->setVertexBytes(&uniforms, sizeof(Uniforms), 0);

        // Set vertex buffer (buffer index 1, offset 0 in buffer)
        m_currentEncoder->setVertexBuffer(geom.vertexBuffer.get(), 0, 1);

        // Set texture - always bind a texture (use default white if none provided)
        bool hasTexture = (texture != 0);
        if (hasTexture) {
            auto texIt = m_textures.find(texture);
            if (texIt != m_textures.end()) {
                m_currentEncoder->setFragmentTexture(texIt->second.texture.get(), 0);
            } else {
                // Texture handle provided but texture not found - use default white
                if (m_defaultWhiteTexture) {
                    m_currentEncoder->setFragmentTexture(m_defaultWhiteTexture.get(), 0);
                }
            }
        } else {
            // No texture handle - use default white texture
            if (m_defaultWhiteTexture) {
                m_currentEncoder->setFragmentTexture(m_defaultWhiteTexture.get(), 0);
            }
        }

        // Setup scissor test (scale from logical to framebuffer coordinates)
        if (m_scissor.enabled) {
            int fbHeight = static_cast<int>(m_logicalHeight * m_scaleY);
            MTL::ScissorRect scissorRect;
            scissorRect.x = static_cast<NS::UInteger>(m_scissor.x * m_scaleX);
            scissorRect.y = static_cast<NS::UInteger>(fbHeight - (m_scissor.y + m_scissor.height) * m_scaleY);// Flip Y
            scissorRect.width = static_cast<NS::UInteger>(m_scissor.width * m_scaleX);
            scissorRect.height = static_cast<NS::UInteger>(m_scissor.height * m_scaleY);
            m_currentEncoder->setScissorRect(scissorRect);
        }

        // Draw
        if (geom.indexCount > 0) {
            m_currentEncoder->drawIndexedPrimitives(
                MTL::PrimitiveTypeTriangle, geom.indexCount, MTL::IndexTypeUInt32, geom.indexBuffer.get(), 0
            );
            fmt::print("RmlUiRenderer::RenderGeometry: Drew {} indices\n", geom.indexCount);
        } else {
            fmt::print("RmlUiRenderer::RenderGeometry: Warning - index count is 0\n");
        }
    }

    void RmlUiRenderer::ReleaseGeometry(Rml::CompiledGeometryHandle geometry) {
        auto it = m_geometry.find(geometry);
        if (it != m_geometry.end()) {
            m_geometry.erase(it);
        }
    }

    void RmlUiRenderer::EnableScissorRegion(bool enable) {
        m_scissor.enabled = enable;
    }

    void RmlUiRenderer::SetScissorRegion(Rml::Rectanglei region) {
        m_scissor.x = region.Left();
        m_scissor.y = region.Top();
        m_scissor.width = region.Width();
        m_scissor.height = region.Height();
    }

    Rml::TextureHandle RmlUiRenderer::LoadTexture(Rml::Vector2i& texture_dimensions, const Rml::String& source) {
        // For now, we'll just log that a texture was requested
        // In a full implementation, you'd load the image file here
        fmt::print("RmlUiRenderer::LoadTexture: Requested texture: {}\n", source.c_str());
        // Return 0 to indicate no texture (RmlUI will use a default white texture)
        return 0;
    }

    Rml::TextureHandle
        RmlUiRenderer::GenerateTexture(Rml::Span<const Rml::byte> source, Rml::Vector2i source_dimensions) {
        if (!m_device) {
            // spdlog::error("RmlUiRenderer::GenerateTexture: Device not initialized");
            return 0;
        }

        // Create texture descriptor
        auto texDesc = NS::TransferPtr(MTL::TextureDescriptor::alloc()->init());
        texDesc->setTextureType(MTL::TextureType2D);
        texDesc->setPixelFormat(MTL::PixelFormatRGBA8Unorm);
        texDesc->setWidth(source_dimensions.x);
        texDesc->setHeight(source_dimensions.y);
        texDesc->setUsage(MTL::TextureUsageShaderRead);
        texDesc->setStorageMode(MTL::StorageModeShared);

        auto texture = NS::TransferPtr(m_device->newTexture(texDesc.get()));
        if (!texture) {
            // spdlog::error("RmlUiRenderer::GenerateTexture: Failed to create texture");
            return 0;
        }

        // Upload texture data
        MTL::Region region(0, 0, 0, source_dimensions.x, source_dimensions.y, 1);
        texture->replaceRegion(region, 0, source.data(), source_dimensions.x * 4);

        TextureData texData;
        texData.texture = texture;
        texData.width = source_dimensions.x;
        texData.height = source_dimensions.y;

        Rml::TextureHandle textureHandle = m_nextTextureHandle++;
        m_textures[textureHandle] = std::move(texData);

        return textureHandle;
    }

    void RmlUiRenderer::ReleaseTexture(Rml::TextureHandle texture_handle) {
        auto it = m_textures.find(texture_handle);
        if (it != m_textures.end()) {
            m_textures.erase(it);
        }
    }

    void RmlUiRenderer::SetTransform(const Rml::Matrix4f* transform) {
        if (transform) {
            m_transform = *transform;
        } else {
            m_transform = Rml::Matrix4f::Identity();
        }
    }

    void RmlUiRenderer::CreateDefaultWhiteTexture() {
        if (!m_device) {
            return;
        }

        // Create a 1x1 white texture
        auto texDesc = NS::TransferPtr(MTL::TextureDescriptor::alloc()->init());
        texDesc->setTextureType(MTL::TextureType2D);
        texDesc->setPixelFormat(MTL::PixelFormatRGBA8Unorm);
        texDesc->setWidth(1);
        texDesc->setHeight(1);
        texDesc->setUsage(MTL::TextureUsageShaderRead);
        texDesc->setStorageMode(MTL::StorageModeShared);

        m_defaultWhiteTexture = NS::TransferPtr(m_device->newTexture(texDesc.get()));
        if (m_defaultWhiteTexture) {
            // Fill with white (255, 255, 255, 255)
            uint8_t whitePixel[4] = { 255, 255, 255, 255 };
            MTL::Region region(0, 0, 0, 1, 1, 1);
            m_defaultWhiteTexture->replaceRegion(region, 0, whitePixel, 4);
            fmt::print("RmlUiRenderer::CreateDefaultWhiteTexture: Created default white texture\n");
        }
    }

    void RmlUiRenderer::CreatePipelineState() {
        if (!m_device) {
            // spdlog::error("RmlUiRenderer::CreatePipelineState: Device not initialized");
            return;
        }

        // Load shader source
        std::string shaderSrc;
        try {
            shaderSrc = readFile("assets/shaders/rmlui.metal");
        } catch (const std::exception& e) {
            // spdlog::error("RmlUiRenderer::CreatePipelineState: Failed to load shader: {}", e.what());
            return;
        }

        // Compile shader library
        auto code = NS::String::string(shaderSrc.data(), NS::StringEncoding::UTF8StringEncoding);
        NS::Error* error = nullptr;
        MTL::Library* library = m_device->newLibrary(code, nullptr, &error);
        if (!library) {
            // spdlog::error(
            //     "RmlUiRenderer::CreatePipelineState: Failed to compile shader: {}",
            //     error ? error->localizedDescription()->utf8String() : "Unknown error"
            // );
            return;
        }

        // Get shader functions
        auto vertexFunc =
            library->newFunction(NS::String::string("vertexMain", NS::StringEncoding::UTF8StringEncoding));
        auto fragmentFunc =
            library->newFunction(NS::String::string("fragmentMain", NS::StringEncoding::UTF8StringEncoding));

        if (!vertexFunc || !fragmentFunc) {
            // spdlog::error("RmlUiRenderer::CreatePipelineState: Failed to get shader functions");
            library->release();
            return;
        }

        // Create vertex descriptor
        auto vertexDesc = NS::TransferPtr(MTL::VertexDescriptor::alloc()->init());

        // Position (attribute 0): float2
        auto posAttr = vertexDesc->attributes()->object(0);
        posAttr->setFormat(MTL::VertexFormatFloat2);
        posAttr->setOffset(offsetof(Rml::Vertex, position));
        posAttr->setBufferIndex(1);// Changed from 0 to 1 to avoid conflict with uniforms

        // Color (attribute 1): uchar4 normalized
        auto colorAttr = vertexDesc->attributes()->object(1);
        colorAttr->setFormat(MTL::VertexFormatUChar4Normalized);
        colorAttr->setOffset(offsetof(Rml::Vertex, colour));
        colorAttr->setBufferIndex(1);// Changed from 0 to 1

        // TexCoord (attribute 2): float2
        auto texAttr = vertexDesc->attributes()->object(2);
        texAttr->setFormat(MTL::VertexFormatFloat2);
        texAttr->setOffset(offsetof(Rml::Vertex, tex_coord));
        texAttr->setBufferIndex(1);// Changed from 0 to 1

        // Layout for buffer index 1 (vertex data)
        auto layout = vertexDesc->layouts()->object(1);// Changed from 0 to 1
        layout->setStride(sizeof(Rml::Vertex));
        layout->setStepFunction(MTL::VertexStepFunctionPerVertex);
        layout->setStepRate(1);

        // Create pipeline descriptor
        auto pipelineDesc = NS::TransferPtr(MTL::RenderPipelineDescriptor::alloc()->init());
        pipelineDesc->setVertexFunction(vertexFunc);
        pipelineDesc->setFragmentFunction(fragmentFunc);
        pipelineDesc->setVertexDescriptor(vertexDesc.get());

        // Color attachment
        auto colorAttachment = pipelineDesc->colorAttachments()->object(0);
        colorAttachment->setPixelFormat(MTL::PixelFormatRGBA8Unorm_sRGB);
        colorAttachment->setBlendingEnabled(true);
        colorAttachment->setRgbBlendOperation(MTL::BlendOperationAdd);
        colorAttachment->setAlphaBlendOperation(MTL::BlendOperationAdd);
        colorAttachment->setSourceRGBBlendFactor(MTL::BlendFactorSourceAlpha);
        colorAttachment->setSourceAlphaBlendFactor(MTL::BlendFactorSourceAlpha);
        colorAttachment->setDestinationRGBBlendFactor(MTL::BlendFactorOneMinusSourceAlpha);
        colorAttachment->setDestinationAlphaBlendFactor(MTL::BlendFactorOneMinusSourceAlpha);

        // Create pipeline state
        m_pipelineState = NS::TransferPtr(m_device->newRenderPipelineState(pipelineDesc.get(), &error));
        if (!m_pipelineState) {
            // spdlog::error(
            //     "RmlUiRenderer::CreatePipelineState: Failed to create pipeline state: {}",
            //     error ? error->localizedDescription()->utf8String() : "Unknown error"
            // );
        }

        // Create depth stencil state (disabled for UI)
        auto depthStencilDesc = NS::TransferPtr(MTL::DepthStencilDescriptor::alloc()->init());
        depthStencilDesc->setDepthCompareFunction(MTL::CompareFunctionAlways);
        depthStencilDesc->setDepthWriteEnabled(false);
        m_depthStencilState = NS::TransferPtr(m_device->newDepthStencilState(depthStencilDesc.get()));

        // Cleanup
        vertexFunc->release();
        fragmentFunc->release();
        library->release();
    }

}// namespace Vapor
