#include <memory>
#define NS_PRIVATE_IMPLEMENTATION
#define MTL_PRIVATE_IMPLEMENTATION
#define CA_PRIVATE_IMPLEMENTATION
#include "debug_draw.hpp"
#include "renderer_metal.hpp"

#include <SDL3/SDL_stdinc.h>
#include <SDL3/SDL_timer.h>
#include <fmt/core.h>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>
#include <tracy/Tracy.hpp>
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#define GLM_FORCE_LEFT_HANDED
#include "backends/imgui_impl_metal.h"
#include "backends/imgui_impl_sdl3.h"
#include <cstdlib>
#include <ctime>
#include <functional>
#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>
#include <imgui.h>
#include <vector>

#include "asset_manager.hpp"
#include "engine_core.hpp"
#include "graphics.hpp"
#include "helper.hpp"
#include "mesh_builder.hpp"
#include "rmlui_manager.hpp"

#include <RmlUi/Core.h>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

// GIBS (Global Illumination Based on Surfels)
#include "Vapor/gibs_manager.hpp"
#include "Vapor/gibs_passes.hpp"

namespace Vapor {

    class RmlUiRenderer_Metal : public Rml::RenderInterface {
    public:
        explicit RmlUiRenderer_Metal(MTL::Device* device) : m_device(device) {
            m_transform = Rml::Matrix4f::Identity();
        }

        ~RmlUiRenderer_Metal() override {
            Shutdown();
        }

        bool Initialize() {
            if (!m_device) {
                return false;
            }
            CreateDefaultWhiteTexture();
            CreatePipelineState();
            return true;
        }

        void Shutdown() {
            m_geometry.clear();
            m_textures.clear();
            m_pipelineState.reset();
            m_depthStencilState.reset();
            m_defaultWhiteTexture.reset();
        }

        void BeginFrame(MTL::CommandBuffer* commandBuffer, MTL::Texture* renderTarget, int width, int height) {
            if (!commandBuffer || !renderTarget) {
                return;
            }

            // width/height are logical (window) size for RmlUI coordinates / projection
            m_logicalWidth = width;
            m_logicalHeight = height;

            // Get framebuffer size and calculate HiDPI scale
            int fbWidth = static_cast<int>(renderTarget->width());
            int fbHeight = static_cast<int>(renderTarget->height());
            m_scaleX = width > 0 ? static_cast<float>(fbWidth) / width : 1.0f;
            m_scaleY = height > 0 ? static_cast<float>(fbHeight) / height : 1.0f;

            m_currentCommandBuffer = commandBuffer;
            m_currentRenderTarget = renderTarget;

            // Create render pass descriptor
            m_currentPassDesc = NS::TransferPtr(MTL::RenderPassDescriptor::renderPassDescriptor());
            auto colorAttachment = m_currentPassDesc->colorAttachments()->object(0);
            colorAttachment->setTexture(renderTarget);
            colorAttachment->setLoadAction(MTL::LoadActionLoad);// Load existing content
            colorAttachment->setStoreAction(MTL::StoreActionStore);

            m_currentEncoder = commandBuffer->renderCommandEncoder(m_currentPassDesc.get());

            // Set viewport to framebuffer size
            MTL::Viewport viewport;
            viewport.originX = 0.0;
            viewport.originY = 0.0;
            viewport.width = static_cast<double>(fbWidth);
            viewport.height = static_cast<double>(fbHeight);
            viewport.znear = 0.0;
            viewport.zfar = 1.0;
            m_currentEncoder->setViewport(viewport);

            // Set scissor rect to full framebuffer
            MTL::ScissorRect scissorRect;
            scissorRect.x = 0;
            scissorRect.y = 0;
            scissorRect.width = static_cast<NS::UInteger>(fbWidth);
            scissorRect.height = static_cast<NS::UInteger>(fbHeight);
            m_currentEncoder->setScissorRect(scissorRect);
        }

        void EndFrame() {
            if (m_currentEncoder) {
                m_currentEncoder->endEncoding();
                m_currentEncoder = nullptr;
            }
            m_currentCommandBuffer = nullptr;
            m_currentRenderTarget = nullptr;
            m_currentPassDesc.reset();
        }

        // Rml::RenderInterface implementation
        Rml::CompiledGeometryHandle
            CompileGeometry(Rml::Span<const Rml::Vertex> vertices, Rml::Span<const int> indices) override {
            if (!m_device) {
                return 0;
            }

            // Create vertex buffer
            auto vertexBuffer = NS::TransferPtr(
                m_device->newBuffer(vertices.size() * sizeof(Rml::Vertex), MTL::ResourceStorageModeShared)
            );
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

        void RenderGeometry(Rml::CompiledGeometryHandle geometry, Rml::Vector2f translation, Rml::TextureHandle texture)
            override {
            if (!m_currentEncoder) {
                return;
            }

            auto it = m_geometry.find(geometry);
            if (it == m_geometry.end()) {
                return;
            }

            const CompiledGeometry& geom = it->second;

            // Set pipeline state
            if (!m_pipelineState) {
                return;
            }
            m_currentEncoder->setRenderPipelineState(m_pipelineState.get());
            m_currentEncoder->setDepthStencilState(m_depthStencilState.get());
            m_currentEncoder->setCullMode(MTL::CullModeNone);

            // Calculate projection matrix (use logical size for RmlUI coordinates)
            glm::mat4 projection = glm::ortho(0.0f, (float)m_logicalWidth, (float)m_logicalHeight, 0.0f, -1.0f, 1.0f);

            // Apply translation
            glm::mat4 transform = glm::make_mat4(m_transform.data());
            transform = glm::translate(transform, glm::vec3(translation.x, translation.y, 0.0f));

            // Create uniforms
            struct Uniforms {
                glm::mat4 projectionMatrix;
                glm::mat4 transformMatrix;
            } uniforms;
            uniforms.projectionMatrix = projection;
            uniforms.transformMatrix = transform;

            m_currentEncoder->setVertexBytes(&uniforms, sizeof(Uniforms), 0);
            m_currentEncoder->setVertexBuffer(geom.vertexBuffer.get(), 0, 1);

            // Set texture
            bool hasTexture = (texture != 0);
            if (hasTexture) {
                auto texIt = m_textures.find(texture);
                if (texIt != m_textures.end()) {
                    m_currentEncoder->setFragmentTexture(texIt->second.texture.get(), 0);
                } else if (m_defaultWhiteTexture) {
                    m_currentEncoder->setFragmentTexture(m_defaultWhiteTexture.get(), 0);
                }
            } else if (m_defaultWhiteTexture) {
                m_currentEncoder->setFragmentTexture(m_defaultWhiteTexture.get(), 0);
            }

            // Setup scissor (scale from logical to framebuffer coordinates)
            if (m_scissor.enabled) {
                int fbHeight = static_cast<int>(m_logicalHeight * m_scaleY);
                MTL::ScissorRect scissorRect;
                scissorRect.x = static_cast<NS::UInteger>(m_scissor.x * m_scaleX);
                scissorRect.y = static_cast<NS::UInteger>(fbHeight - (m_scissor.y + m_scissor.height) * m_scaleY);
                scissorRect.width = static_cast<NS::UInteger>(m_scissor.width * m_scaleX);
                scissorRect.height = static_cast<NS::UInteger>(m_scissor.height * m_scaleY);
                m_currentEncoder->setScissorRect(scissorRect);
            }

            // Draw
            if (geom.indexCount > 0) {
                m_currentEncoder->drawIndexedPrimitives(
                    MTL::PrimitiveTypeTriangle, geom.indexCount, MTL::IndexTypeUInt32, geom.indexBuffer.get(), 0
                );
            }
        }

        void ReleaseGeometry(Rml::CompiledGeometryHandle geometry) override {
            m_geometry.erase(geometry);
        }

        void EnableScissorRegion(bool enable) override {
            m_scissor.enabled = enable;
        }

        void SetScissorRegion(Rml::Rectanglei region) override {
            m_scissor.x = region.Left();
            m_scissor.y = region.Top();
            m_scissor.width = region.Width();
            m_scissor.height = region.Height();
        }

        Rml::TextureHandle LoadTexture(Rml::Vector2i& texture_dimensions, const Rml::String& source) override {
            // Not implemented for now
            return 0;
        }

        Rml::TextureHandle
            GenerateTexture(Rml::Span<const Rml::byte> source, Rml::Vector2i source_dimensions) override {
            if (!m_device) {
                return 0;
            }

            auto texDesc = NS::TransferPtr(MTL::TextureDescriptor::alloc()->init());
            texDesc->setTextureType(MTL::TextureType2D);
            texDesc->setPixelFormat(MTL::PixelFormatRGBA8Unorm);
            texDesc->setWidth(source_dimensions.x);
            texDesc->setHeight(source_dimensions.y);
            texDesc->setUsage(MTL::TextureUsageShaderRead);
            texDesc->setStorageMode(MTL::StorageModeShared);

            auto texture = NS::TransferPtr(m_device->newTexture(texDesc.get()));
            if (!texture) {
                return 0;
            }

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

        void ReleaseTexture(Rml::TextureHandle texture_handle) override {
            m_textures.erase(texture_handle);
        }

        void SetTransform(const Rml::Matrix4f* transform) override {
            if (transform) {
                m_transform = *transform;
            } else {
                m_transform = Rml::Matrix4f::Identity();
            }
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

        void CreateDefaultWhiteTexture() {
            if (!m_device) {
                return;
            }

            auto texDesc = NS::TransferPtr(MTL::TextureDescriptor::alloc()->init());
            texDesc->setTextureType(MTL::TextureType2D);
            texDesc->setPixelFormat(MTL::PixelFormatRGBA8Unorm);
            texDesc->setWidth(1);
            texDesc->setHeight(1);
            texDesc->setUsage(MTL::TextureUsageShaderRead);
            texDesc->setStorageMode(MTL::StorageModeShared);

            m_defaultWhiteTexture = NS::TransferPtr(m_device->newTexture(texDesc.get()));
            if (m_defaultWhiteTexture) {
                uint8_t whitePixel[4] = { 255, 255, 255, 255 };
                MTL::Region region(0, 0, 0, 1, 1, 1);
                m_defaultWhiteTexture->replaceRegion(region, 0, whitePixel, 4);
            }
        }

        void CreatePipelineState() {
            if (!m_device) {
                return;
            }

            // Load shader
            std::string shaderSrc = readFile("assets/shaders/rmlui.metal");
            auto code = NS::String::string(shaderSrc.data(), NS::StringEncoding::UTF8StringEncoding);
            NS::Error* error = nullptr;
            MTL::Library* library = m_device->newLibrary(code, nullptr, &error);
            if (!library) {
                return;
            }

            auto vertexFunc =
                library->newFunction(NS::String::string("vertexMain", NS::StringEncoding::UTF8StringEncoding));
            auto fragmentFunc =
                library->newFunction(NS::String::string("fragmentMain", NS::StringEncoding::UTF8StringEncoding));

            if (!vertexFunc || !fragmentFunc) {
                library->release();
                return;
            }

            // Create vertex descriptor
            auto vertexDesc = NS::TransferPtr(MTL::VertexDescriptor::alloc()->init());

            auto posAttr = vertexDesc->attributes()->object(0);
            posAttr->setFormat(MTL::VertexFormatFloat2);
            posAttr->setOffset(offsetof(Rml::Vertex, position));
            posAttr->setBufferIndex(1);

            auto colorAttr = vertexDesc->attributes()->object(1);
            colorAttr->setFormat(MTL::VertexFormatUChar4Normalized);
            colorAttr->setOffset(offsetof(Rml::Vertex, colour));
            colorAttr->setBufferIndex(1);

            auto texAttr = vertexDesc->attributes()->object(2);
            texAttr->setFormat(MTL::VertexFormatFloat2);
            texAttr->setOffset(offsetof(Rml::Vertex, tex_coord));
            texAttr->setBufferIndex(1);

            auto layout = vertexDesc->layouts()->object(1);
            layout->setStride(sizeof(Rml::Vertex));
            layout->setStepFunction(MTL::VertexStepFunctionPerVertex);
            layout->setStepRate(1);

            // Create pipeline descriptor
            auto pipelineDesc = NS::TransferPtr(MTL::RenderPipelineDescriptor::alloc()->init());
            pipelineDesc->setVertexFunction(vertexFunc);
            pipelineDesc->setFragmentFunction(fragmentFunc);
            pipelineDesc->setVertexDescriptor(vertexDesc.get());

            auto colorAttachment = pipelineDesc->colorAttachments()->object(0);
            colorAttachment->setPixelFormat(MTL::PixelFormatRGBA8Unorm_sRGB);
            colorAttachment->setBlendingEnabled(true);
            colorAttachment->setRgbBlendOperation(MTL::BlendOperationAdd);
            colorAttachment->setAlphaBlendOperation(MTL::BlendOperationAdd);
            colorAttachment->setSourceRGBBlendFactor(MTL::BlendFactorSourceAlpha);
            colorAttachment->setSourceAlphaBlendFactor(MTL::BlendFactorSourceAlpha);
            colorAttachment->setDestinationRGBBlendFactor(MTL::BlendFactorOneMinusSourceAlpha);
            colorAttachment->setDestinationAlphaBlendFactor(MTL::BlendFactorOneMinusSourceAlpha);

            m_pipelineState = NS::TransferPtr(m_device->newRenderPipelineState(pipelineDesc.get(), &error));

            // Create depth stencil state
            auto depthStencilDesc = NS::TransferPtr(MTL::DepthStencilDescriptor::alloc()->init());
            depthStencilDesc->setDepthCompareFunction(MTL::CompareFunctionAlways);
            depthStencilDesc->setDepthWriteEnabled(false);
            m_depthStencilState = NS::TransferPtr(m_device->newDepthStencilState(depthStencilDesc.get()));

            vertexFunc->release();
            fragmentFunc->release();
            library->release();
        }

        MTL::Device* m_device = nullptr;
        MTL::CommandBuffer* m_currentCommandBuffer = nullptr;
        MTL::RenderCommandEncoder* m_currentEncoder = nullptr;
        MTL::Texture* m_currentRenderTarget = nullptr;
        NS::SharedPtr<MTL::RenderPassDescriptor> m_currentPassDesc;

        NS::SharedPtr<MTL::RenderPipelineState> m_pipelineState;
        NS::SharedPtr<MTL::DepthStencilState> m_depthStencilState;
        NS::SharedPtr<MTL::Texture> m_defaultWhiteTexture;

        std::unordered_map<Rml::CompiledGeometryHandle, CompiledGeometry> m_geometry;
        std::unordered_map<Rml::TextureHandle, TextureData> m_textures;

        Rml::CompiledGeometryHandle m_nextGeometryHandle = 1;
        Rml::TextureHandle m_nextTextureHandle = 1;

        int m_logicalWidth = 0;
        int m_logicalHeight = 0;
        float m_scaleX = 1.0f;
        float m_scaleY = 1.0f;

        struct ScissorRegion {
            bool enabled = false;
            int x = 0, y = 0, width = 0, height = 0;
        } m_scissor;

        Rml::Matrix4f m_transform;
    };

}// namespace Vapor

// Pre-pass: Renders depth and normals
class PrePass : public RenderPass {
public:
    explicit PrePass(Renderer_Metal* renderer) : RenderPass(renderer) {
    }

    const char* getName() const override {
        return "PrePass";
    }

    void execute() override {
        auto& r = *renderer;

        // Create render pass descriptor
        auto prePassDesc = NS::TransferPtr(MTL::RenderPassDescriptor::renderPassDescriptor());
        auto prePassNormalRT = prePassDesc->colorAttachments()->object(0);
        prePassNormalRT->setClearColor(MTL::ClearColor(0.0, 0.0, 0.0, 1.0));
        prePassNormalRT->setLoadAction(MTL::LoadActionClear);
        prePassNormalRT->setStoreAction(MTL::StoreActionStore);
        prePassNormalRT->setTexture(r.normalRT_MS.get());

        auto prePassDepthRT = prePassDesc->depthAttachment();
        prePassDepthRT->setClearDepth(r.clearDepth);
        prePassDepthRT->setLoadAction(MTL::LoadActionClear);
        prePassDepthRT->setStoreAction(MTL::StoreActionStoreAndMultisampleResolve);
        prePassDepthRT->setDepthResolveFilter(MTL::MultisampleDepthResolveFilter::MultisampleDepthResolveFilterMin);
        prePassDepthRT->setTexture(r.depthStencilRT_MS.get());
        prePassDepthRT->setResolveTexture(r.depthStencilRT.get());

        // Execute the pass
        auto encoder = r.currentCommandBuffer->renderCommandEncoder(prePassDesc.get());
        encoder->setRenderPipelineState(r.prePassPipeline.get());
        encoder->setCullMode(MTL::CullModeBack);
        encoder->setFrontFacingWinding(MTL::Winding::WindingCounterClockwise);
        encoder->setDepthStencilState(r.depthStencilState.get());

        encoder->setVertexBuffer(r.cameraDataBuffers[r.currentFrameInFlight].get(), 0, 0);
        encoder->setVertexBuffer(r.materialDataBuffer.get(), 0, 1);
        encoder->setVertexBuffer(r.instanceDataBuffers[r.currentFrameInFlight].get(), 0, 2);
        encoder->setVertexBuffer(r.getBuffer(r.currentScene->vertexBuffer).get(), 0, 3);

        for (const auto& [material, meshes] : r.instanceBatches) {
            encoder->setFragmentTexture(
                r.getTexture(material->albedoMap ? material->albedoMap->texture : r.defaultAlbedoTexture).get(), 0
            );

            for (const auto& mesh : meshes) {
                if (!r.currentCamera->isVisible(mesh->getWorldBoundingSphere())) {
                    continue;
                }

                encoder->setVertexBytes(&mesh->instanceID, sizeof(Uint32), 4);
                encoder->drawIndexedPrimitives(
                    MTL::PrimitiveType::PrimitiveTypeTriangle,
                    mesh->indexCount,
                    MTL::IndexTypeUInt32,
                    r.getBuffer(r.currentScene->indexBuffer).get(),
                    mesh->indexOffset * sizeof(Uint32)
                );
                r.drawCount++;
            }
        }

        encoder->endEncoding();
    }
};

// TLAS build pass: Builds top-level acceleration structure for ray tracing
class TLASBuildPass : public RenderPass {
public:
    explicit TLASBuildPass(Renderer_Metal* renderer) : RenderPass(renderer) {
    }

    const char* getName() const override {
        return "TLASBuildPass";
    }

    void execute() override {
        auto& r = *renderer;

        // Build BLAS array if geometry is dirty
        if (r.currentScene->isGeometryDirty) {
            std::vector<NS::Object*> BLASObjects;
            BLASObjects.reserve(r.BLASs.size());
            for (auto blas : r.BLASs) {
                BLASObjects.push_back(static_cast<NS::Object*>(blas.get()));
            }
            r.BLASArray = NS::TransferPtr(NS::Array::array(BLASObjects.data(), BLASObjects.size()));
            r.currentScene->isGeometryDirty = false;
        }

        // Create TLAS descriptor
        auto TLASDesc = NS::TransferPtr(MTL::InstanceAccelerationStructureDescriptor::alloc()->init());
        TLASDesc->setInstanceCount(r.accelInstances.size());
        TLASDesc->setInstancedAccelerationStructures(r.BLASArray.get());
        TLASDesc->setInstanceDescriptorBuffer(r.accelInstanceBuffers[r.currentFrameInFlight].get());

        auto TLASSizes = r.device->accelerationStructureSizes(TLASDesc.get());
        if (!r.TLASScratchBuffers[r.currentFrameInFlight]
            || r.TLASScratchBuffers[r.currentFrameInFlight]->length() < TLASSizes.buildScratchBufferSize) {
            r.TLASScratchBuffers[r.currentFrameInFlight] =
                NS::TransferPtr(r.device->newBuffer(TLASSizes.buildScratchBufferSize, MTL::ResourceStorageModePrivate));
        }
        if (!r.TLASBuffers[r.currentFrameInFlight]
            || r.TLASBuffers[r.currentFrameInFlight]->size() < TLASSizes.accelerationStructureSize) {
            r.TLASBuffers[r.currentFrameInFlight] =
                NS::TransferPtr(r.device->newAccelerationStructure(TLASSizes.accelerationStructureSize));
        }

        // Build TLAS
        // TODO: only build TLAS if it's dirty
        auto accelEncoder = r.currentCommandBuffer->accelerationStructureCommandEncoder();
        accelEncoder->buildAccelerationStructure(
            r.TLASBuffers[r.currentFrameInFlight].get(),
            TLASDesc.get(),
            r.TLASScratchBuffers[r.currentFrameInFlight].get(),
            0
        );
        accelEncoder->endEncoding();
    }
};

// Normal resolve pass: Resolves MSAA normal texture
class NormalResolvePass : public RenderPass {
public:
    explicit NormalResolvePass(Renderer_Metal* renderer) : RenderPass(renderer) {
    }

    const char* getName() const override {
        return "NormalResolvePass";
    }

    void execute() override {
        auto& r = *renderer;

        auto drawableSize = r.swapchain->drawableSize();
        glm::vec2 screenSize = glm::vec2(drawableSize.width, drawableSize.height);

        auto encoder = r.currentCommandBuffer->computeCommandEncoder();
        encoder->setComputePipelineState(r.normalResolvePipeline.get());
        encoder->setTexture(r.normalRT_MS.get(), 0);
        encoder->setTexture(r.normalRT.get(), 1);
        encoder->setBytes(&r.MSAA_SAMPLE_COUNT, sizeof(Uint32), 0);
        encoder->dispatchThreadgroups(MTL::Size(screenSize.x, screenSize.y, 1), MTL::Size(1, 1, 1));
        encoder->endEncoding();
    }
};

// Tile culling pass: Performs light culling for tiled rendering
class TileCullingPass : public RenderPass {
public:
    explicit TileCullingPass(Renderer_Metal* renderer) : RenderPass(renderer) {
    }

    const char* getName() const override {
        return "TileCullingPass";
    }

    void execute() override {
        auto& r = *renderer;

        auto drawableSize = r.swapchain->drawableSize();
        glm::vec2 screenSize = glm::vec2(drawableSize.width, drawableSize.height);
        glm::uvec3 gridSize = glm::uvec3(r.clusterGridSizeX, r.clusterGridSizeY, r.clusterGridSizeZ);
        uint pointLightCount = r.currentScene->pointLights.size();

        auto encoder = r.currentCommandBuffer->computeCommandEncoder();
        encoder->setComputePipelineState(r.tileCullingPipeline.get());
        encoder->setBuffer(r.clusterBuffers[r.currentFrameInFlight].get(), 0, 0);
        encoder->setBuffer(r.pointLightBuffer.get(), 0, 1);
        encoder->setBuffer(r.cameraDataBuffers[r.currentFrameInFlight].get(), 0, 2);
        encoder->setBytes(&pointLightCount, sizeof(uint), 3);
        encoder->setBytes(&gridSize, sizeof(glm::uvec3), 4);
        encoder->setBytes(&screenSize, sizeof(glm::vec2), 5);
        encoder->dispatchThreadgroups(MTL::Size(r.clusterGridSizeX, r.clusterGridSizeY, 1), MTL::Size(1, 1, 1));
        encoder->endEncoding();
    }
};

// Raytrace shadow pass: Computes ray-traced shadows
class RaytraceShadowPass : public RenderPass {
public:
    explicit RaytraceShadowPass(Renderer_Metal* renderer) : RenderPass(renderer) {
    }

    const char* getName() const override {
        return "RaytraceShadowPass";
    }

    void execute() override {
        auto& r = *renderer;

        auto drawableSize = r.swapchain->drawableSize();
        glm::vec2 screenSize = glm::vec2(drawableSize.width, drawableSize.height);

        auto encoder = r.currentCommandBuffer->computeCommandEncoder();
        encoder->setComputePipelineState(r.raytraceShadowPipeline.get());
        encoder->setTexture(r.depthStencilRT.get(), 0);
        encoder->setTexture(r.normalRT.get(), 1);
        encoder->setTexture(r.shadowRT.get(), 2);
        encoder->setBuffer(r.cameraDataBuffers[r.currentFrameInFlight].get(), 0, 0);
        encoder->setBuffer(r.directionalLightBuffer.get(), 0, 1);
        encoder->setBuffer(r.pointLightBuffer.get(), 0, 2);
        encoder->setBytes(&screenSize, sizeof(glm::vec2), 3);
        encoder->setAccelerationStructure(r.TLASBuffers[r.currentFrameInFlight].get(), 4);
        encoder->dispatchThreadgroups(MTL::Size(screenSize.x, screenSize.y, 1), MTL::Size(1, 1, 1));
        encoder->endEncoding();

        // Generate mipmaps for shadow texture
        auto mipmapEncoder = NS::TransferPtr(r.currentCommandBuffer->blitCommandEncoder());
        mipmapEncoder->generateMipmaps(r.shadowRT.get());
        mipmapEncoder->endEncoding();
    }
};

// Raytrace AO pass: Computes ray-traced ambient occlusion
class RaytraceAOPass : public RenderPass {
public:
    explicit RaytraceAOPass(Renderer_Metal* renderer) : RenderPass(renderer) {
    }

    const char* getName() const override {
        return "RaytraceAOPass";
    }

    void execute() override {
        auto& r = *renderer;

        auto drawableSize = r.swapchain->drawableSize();
        glm::vec2 screenSize = glm::vec2(drawableSize.width, drawableSize.height);

        auto encoder = r.currentCommandBuffer->computeCommandEncoder();
        encoder->setComputePipelineState(r.raytraceAOPipeline.get());
        encoder->setTexture(r.depthStencilRT.get(), 0);
        encoder->setTexture(r.normalRT.get(), 1);
        encoder->setTexture(r.aoRT.get(), 2);
        encoder->setBuffer(r.frameDataBuffers[r.currentFrameInFlight].get(), 0, 0);
        encoder->setBuffer(r.cameraDataBuffers[r.currentFrameInFlight].get(), 0, 1);
        encoder->setAccelerationStructure(r.TLASBuffers[r.currentFrameInFlight].get(), 2);
        encoder->dispatchThreadgroups(MTL::Size(screenSize.x, screenSize.y, 1), MTL::Size(1, 1, 1));
        encoder->endEncoding();
    }
};

// Sky atmosphere pass: Renders procedural sky with Rayleigh and Mie scattering
class SkyAtmospherePass : public RenderPass {
public:
    explicit SkyAtmospherePass(Renderer_Metal* renderer) : RenderPass(renderer) {
    }

    const char* getName() const override {
        return "SkyAtmospherePass";
    }

    void execute() override {
        auto& r = *renderer;

        // Create render pass descriptor - render to color RT with blending
        auto skyPassDesc = NS::TransferPtr(MTL::RenderPassDescriptor::renderPassDescriptor());
        auto skyPassColorRT = skyPassDesc->colorAttachments()->object(0);
        skyPassColorRT->setLoadAction(MTL::LoadActionLoad);// Preserve existing scene content
        skyPassColorRT->setStoreAction(MTL::StoreActionStore);
        skyPassColorRT->setTexture(r.colorRT.get());

        // Set depth attachment - use resolved depth from MainRenderPass
        auto skyPassDepthRT = skyPassDesc->depthAttachment();
        skyPassDepthRT->setLoadAction(MTL::LoadActionLoad);
        skyPassDepthRT->setStoreAction(MTL::StoreActionDontCare);// Don't write depth
        skyPassDepthRT->setTexture(r.depthStencilRT.get());

        // Execute the pass
        auto encoder = r.currentCommandBuffer->renderCommandEncoder(skyPassDesc.get());
        encoder->setRenderPipelineState(r.atmospherePipeline.get());
        encoder->setCullMode(MTL::CullModeNone);

        // Use hardware depth test: only render sky where depth == 1.0 (far plane, no geometry)
        // CompareFunctionEqual: sky depth (1.0) == depth buffer (1.0) -> pass, render sky
        //                      sky depth (1.0) == depth buffer (0.5) -> fail, don't render (preserves scene)
        MTL::DepthStencilDescriptor* skyDepthDesc = MTL::DepthStencilDescriptor::alloc()->init();
        skyDepthDesc->setDepthCompareFunction(MTL::CompareFunction::CompareFunctionEqual
        );// Only pass when depth == 1.0 (far plane)
        skyDepthDesc->setDepthWriteEnabled(false);// Don't write depth for sky
        NS::SharedPtr<MTL::DepthStencilState> skyDepthState =
            NS::TransferPtr(r.device->newDepthStencilState(skyDepthDesc));
        skyDepthDesc->release();
        encoder->setDepthStencilState(skyDepthState.get());

        // Set buffers
        encoder->setFragmentBuffer(r.cameraDataBuffers[r.currentFrameInFlight].get(), 0, 0);
        encoder->setFragmentBuffer(r.atmosphereDataBuffer.get(), 0, 1);

        // Draw full-screen triangle
        encoder->drawPrimitives(MTL::PrimitiveType::PrimitiveTypeTriangle, 0, 3, 1);
        encoder->endEncoding();
    }
};

// Sky capture pass: Captures atmosphere to environment cubemap for IBL
class SkyCapturePass : public RenderPass {
public:
    explicit SkyCapturePass(Renderer_Metal* renderer) : RenderPass(renderer) {
    }

    const char* getName() const override {
        return "SkyCapturePass";
    }

    void execute() override {
        auto& r = *renderer;

        if (!r.iblNeedsUpdate) return;

        // Cubemap face view matrices (looking outward from origin)
        const glm::mat4 captureViews[6] = {
            glm::lookAt(glm::vec3(0), glm::vec3(1, 0, 0), glm::vec3(0, -1, 0)),// +X
            glm::lookAt(glm::vec3(0), glm::vec3(-1, 0, 0), glm::vec3(0, -1, 0)),// -X
            glm::lookAt(glm::vec3(0), glm::vec3(0, 1, 0), glm::vec3(0, 0, 1)),// +Y
            glm::lookAt(glm::vec3(0), glm::vec3(0, -1, 0), glm::vec3(0, 0, -1)),// -Y
            glm::lookAt(glm::vec3(0), glm::vec3(0, 0, 1), glm::vec3(0, -1, 0)),// +Z
            glm::lookAt(glm::vec3(0), glm::vec3(0, 0, -1), glm::vec3(0, -1, 0)),// -Z
        };
        const glm::mat4 captureProj = glm::perspective(glm::radians(90.0f), 1.0f, 0.1f, 10.0f);

        // Render each face of the cubemap
        for (uint32_t face = 0; face < 6; ++face) {
            // Update capture data
            IBLCaptureData* captureData = reinterpret_cast<IBLCaptureData*>(r.iblCaptureDataBuffer->contents());
            captureData->viewProj = captureProj * captureViews[face];
            captureData->faceIndex = face;
            captureData->roughness = 0.0f;
            r.iblCaptureDataBuffer->didModifyRange(NS::Range::Make(0, r.iblCaptureDataBuffer->length()));

            // Create render pass for this face
            auto passDesc = NS::TransferPtr(MTL::RenderPassDescriptor::renderPassDescriptor());
            auto colorAttachment = passDesc->colorAttachments()->object(0);
            colorAttachment->setLoadAction(MTL::LoadActionClear);
            colorAttachment->setStoreAction(MTL::StoreActionStore);
            colorAttachment->setClearColor(MTL::ClearColor(0.0, 0.0, 0.0, 1.0));
            colorAttachment->setTexture(r.environmentCubemap.get());
            colorAttachment->setSlice(face);
            colorAttachment->setLevel(0);

            auto encoder = r.currentCommandBuffer->renderCommandEncoder(passDesc.get());
            encoder->setRenderPipelineState(r.skyCapturePipeline.get());
            encoder->setCullMode(MTL::CullModeNone);
            encoder->setVertexBuffer(r.iblCaptureDataBuffer.get(), 0, 0);
            encoder->setFragmentBuffer(r.atmosphereDataBuffer.get(), 0, 0);
            encoder->drawPrimitives(MTL::PrimitiveType::PrimitiveTypeTriangle, 0, 3, 1);
            encoder->endEncoding();
        }

        // Generate mipmaps for environment cubemap
        auto blitEncoder = r.currentCommandBuffer->blitCommandEncoder();
        blitEncoder->generateMipmaps(r.environmentCubemap.get());
        blitEncoder->endEncoding();
    }
};

// Irradiance convolution pass: Creates diffuse irradiance map from environment cubemap
class IrradianceConvolutionPass : public RenderPass {
public:
    explicit IrradianceConvolutionPass(Renderer_Metal* renderer) : RenderPass(renderer) {
    }

    const char* getName() const override {
        return "IrradianceConvolutionPass";
    }

    void execute() override {
        auto& r = *renderer;

        if (!r.iblNeedsUpdate) return;

        // Render each face of the irradiance cubemap
        for (uint32_t face = 0; face < 6; ++face) {
            IBLCaptureData* captureData = reinterpret_cast<IBLCaptureData*>(r.iblCaptureDataBuffer->contents());
            captureData->faceIndex = face;
            captureData->roughness = 0.0f;
            r.iblCaptureDataBuffer->didModifyRange(NS::Range::Make(0, r.iblCaptureDataBuffer->length()));

            auto passDesc = NS::TransferPtr(MTL::RenderPassDescriptor::renderPassDescriptor());
            auto colorAttachment = passDesc->colorAttachments()->object(0);
            colorAttachment->setLoadAction(MTL::LoadActionClear);
            colorAttachment->setStoreAction(MTL::StoreActionStore);
            colorAttachment->setClearColor(MTL::ClearColor(0.0, 0.0, 0.0, 1.0));
            colorAttachment->setTexture(r.irradianceMap.get());
            colorAttachment->setSlice(face);
            colorAttachment->setLevel(0);

            auto encoder = r.currentCommandBuffer->renderCommandEncoder(passDesc.get());
            encoder->setRenderPipelineState(r.irradianceConvolutionPipeline.get());
            encoder->setCullMode(MTL::CullModeNone);
            encoder->setVertexBuffer(r.iblCaptureDataBuffer.get(), 0, 0);
            encoder->setFragmentTexture(r.environmentCubemap.get(), 0);
            encoder->drawPrimitives(MTL::PrimitiveType::PrimitiveTypeTriangle, 0, 3, 1);
            encoder->endEncoding();
        }
    }
};

// Pre-filter environment map pass: Creates specular pre-filtered cubemap with roughness mips
class PrefilterEnvMapPass : public RenderPass {
public:
    explicit PrefilterEnvMapPass(Renderer_Metal* renderer) : RenderPass(renderer) {
    }

    const char* getName() const override {
        return "PrefilterEnvMapPass";
    }

    void execute() override {
        auto& r = *renderer;

        if (!r.iblNeedsUpdate) return;

        const uint32_t maxMipLevels = 5;

        // For each mip level (roughness level)
        for (uint32_t mip = 0; mip < maxMipLevels; ++mip) {
            float roughness = (float)mip / (float)(maxMipLevels - 1);

            // For each face
            for (uint32_t face = 0; face < 6; ++face) {
                IBLCaptureData* captureData = reinterpret_cast<IBLCaptureData*>(r.iblCaptureDataBuffer->contents());
                captureData->faceIndex = face;
                captureData->roughness = roughness;
                r.iblCaptureDataBuffer->didModifyRange(NS::Range::Make(0, r.iblCaptureDataBuffer->length()));

                auto passDesc = NS::TransferPtr(MTL::RenderPassDescriptor::renderPassDescriptor());
                auto colorAttachment = passDesc->colorAttachments()->object(0);
                colorAttachment->setLoadAction(MTL::LoadActionClear);
                colorAttachment->setStoreAction(MTL::StoreActionStore);
                colorAttachment->setClearColor(MTL::ClearColor(0.0, 0.0, 0.0, 1.0));
                colorAttachment->setTexture(r.prefilterMap.get());
                colorAttachment->setSlice(face);
                colorAttachment->setLevel(mip);

                auto encoder = r.currentCommandBuffer->renderCommandEncoder(passDesc.get());
                encoder->setRenderPipelineState(r.prefilterEnvMapPipeline.get());
                encoder->setCullMode(MTL::CullModeNone);
                encoder->setVertexBuffer(r.iblCaptureDataBuffer.get(), 0, 0);
                encoder->setFragmentBuffer(r.iblCaptureDataBuffer.get(), 0, 0);
                encoder->setFragmentTexture(r.environmentCubemap.get(), 0);
                encoder->drawPrimitives(MTL::PrimitiveType::PrimitiveTypeTriangle, 0, 3, 1);
                encoder->endEncoding();
            }
        }
    }
};

// BRDF LUT pass: Pre-computes BRDF integration lookup table
class BRDFLUTPass : public RenderPass {
public:
    explicit BRDFLUTPass(Renderer_Metal* renderer) : RenderPass(renderer) {
    }

    const char* getName() const override {
        return "BRDFLUTPass";
    }

    void execute() override {
        auto& r = *renderer;

        if (!r.iblNeedsUpdate) return;

        auto passDesc = NS::TransferPtr(MTL::RenderPassDescriptor::renderPassDescriptor());
        auto colorAttachment = passDesc->colorAttachments()->object(0);
        colorAttachment->setLoadAction(MTL::LoadActionClear);
        colorAttachment->setStoreAction(MTL::StoreActionStore);
        colorAttachment->setClearColor(MTL::ClearColor(0.0, 0.0, 0.0, 1.0));
        colorAttachment->setTexture(r.brdfLUT.get());

        auto encoder = r.currentCommandBuffer->renderCommandEncoder(passDesc.get());
        encoder->setRenderPipelineState(r.brdfLUTPipeline.get());
        encoder->setCullMode(MTL::CullModeNone);
        encoder->drawPrimitives(MTL::PrimitiveType::PrimitiveTypeTriangle, 0, 3, 1);
        encoder->endEncoding();

        // IBL update complete
        r.iblNeedsUpdate = false;
    }
};

// Main render pass: Renders the scene with PBR lighting
class MainRenderPass : public RenderPass {
public:
    explicit MainRenderPass(Renderer_Metal* renderer) : RenderPass(renderer) {
    }

    const char* getName() const override {
        return "MainRenderPass";
    }

    void execute() override {
        auto& r = *renderer;

        auto drawableSize = r.swapchain->drawableSize();
        glm::vec2 screenSize = glm::vec2(drawableSize.width, drawableSize.height);
        glm::uvec3 gridSize = glm::uvec3(r.clusterGridSizeX, r.clusterGridSizeY, r.clusterGridSizeZ);
        auto time = (float)SDL_GetTicks() / 1000.0f;

        // Create render pass descriptor
        auto renderPassDesc = NS::TransferPtr(MTL::RenderPassDescriptor::renderPassDescriptor());
        auto renderPassColorRT = renderPassDesc->colorAttachments()->object(0);
        renderPassColorRT->setClearColor(MTL::ClearColor(r.clearColor.r, r.clearColor.g, r.clearColor.b, r.clearColor.a)
        );
        renderPassColorRT->setLoadAction(MTL::LoadActionClear);
        renderPassColorRT->setStoreAction(MTL::StoreActionMultisampleResolve);
        renderPassColorRT->setTexture(r.colorRT_MS.get());
        renderPassColorRT->setResolveTexture(r.colorRT.get());

        auto renderPassDepthRT = renderPassDesc->depthAttachment();
        renderPassDepthRT->setLoadAction(MTL::LoadActionLoad);
        renderPassDepthRT->setTexture(r.depthStencilRT_MS.get());

        // Execute the pass
        auto encoder = r.currentCommandBuffer->renderCommandEncoder(renderPassDesc.get());
        encoder->setRenderPipelineState(r.drawPipeline.get());
        encoder->setCullMode(MTL::CullModeBack);
        encoder->setFrontFacingWinding(MTL::Winding::WindingCounterClockwise);
        encoder->setDepthStencilState(r.depthStencilState.get());

        r.currentInstanceCount = 0;
        r.culledInstanceCount = 0;

        encoder->setVertexBuffer(r.cameraDataBuffers[r.currentFrameInFlight].get(), 0, 0);
        encoder->setVertexBuffer(r.materialDataBuffer.get(), 0, 1);
        encoder->setVertexBuffer(r.instanceDataBuffers[r.currentFrameInFlight].get(), 0, 2);
        encoder->setVertexBuffer(r.getBuffer(r.currentScene->vertexBuffer).get(), 0, 3);
        encoder->setFragmentBuffer(r.directionalLightBuffer.get(), 0, 0);
        encoder->setFragmentBuffer(r.pointLightBuffer.get(), 0, 1);
        encoder->setFragmentBuffer(r.clusterBuffers[r.currentFrameInFlight].get(), 0, 2);
        encoder->setFragmentBuffer(r.cameraDataBuffers[r.currentFrameInFlight].get(), 0, 3);
        encoder->setFragmentBytes(&screenSize, sizeof(glm::vec2), 4);
        encoder->setFragmentBytes(&gridSize, sizeof(glm::uvec3), 5);
        encoder->setFragmentBytes(&time, sizeof(float), 6);

        for (const auto& [material, meshes] : r.instanceBatches) {
            encoder->setFragmentTexture(
                r.getTexture(material->albedoMap ? material->albedoMap->texture : r.defaultAlbedoTexture).get(), 0
            );
            encoder->setFragmentTexture(
                r.getTexture(material->normalMap ? material->normalMap->texture : r.defaultNormalTexture).get(), 1
            );
            encoder->setFragmentTexture(
                r.getTexture(material->metallicMap ? material->metallicMap->texture : r.defaultORMTexture).get(), 2
            );
            encoder->setFragmentTexture(
                r.getTexture(material->roughnessMap ? material->roughnessMap->texture : r.defaultORMTexture).get(), 3
            );
            encoder->setFragmentTexture(
                r.getTexture(material->occlusionMap ? material->occlusionMap->texture : r.defaultORMTexture).get(), 4
            );
            encoder->setFragmentTexture(
                r.getTexture(material->emissiveMap ? material->emissiveMap->texture : r.defaultEmissiveTexture).get(), 5
            );
            encoder->setFragmentTexture(r.shadowRT.get(), 7);

            // IBL textures
            encoder->setFragmentTexture(r.irradianceMap.get(), 8);
            encoder->setFragmentTexture(r.prefilterMap.get(), 9);
            encoder->setFragmentTexture(r.brdfLUT.get(), 10);

            // GIBS GI texture
            if (r.gibsEnabled && r.gibsManager && r.gibsManager->getGIResultTexture()) {
                encoder->setFragmentTexture(r.gibsManager->getGIResultTexture(), 11);
            }
            Uint32 gibsEnabledFlag = r.gibsEnabled ? 1 : 0;
            encoder->setFragmentBytes(&gibsEnabledFlag, sizeof(Uint32), 7);

            for (const auto& mesh : meshes) {
                if (!r.currentCamera->isVisible(mesh->getWorldBoundingSphere())) {
                    r.culledInstanceCount++;
                    continue;
                }

                r.currentInstanceCount++;
                encoder->setVertexBytes(&mesh->instanceID, sizeof(Uint32), 4);
                encoder->drawIndexedPrimitives(
                    MTL::PrimitiveType::PrimitiveTypeTriangle,
                    mesh->indexCount,
                    MTL::IndexTypeUInt32,
                    r.getBuffer(r.currentScene->indexBuffer).get(),
                    mesh->indexOffset * sizeof(Uint32)
                );
                r.drawCount++;
            }
        }

        encoder->endEncoding();
    }
};

// Water pass: Renders water surface with Gerstner waves, reflections, and refractions
class WaterPass : public RenderPass {
public:
    explicit WaterPass(Renderer_Metal* renderer) : RenderPass(renderer) {
    }

    const char* getName() const override {
        return "WaterPass";
    }

    void execute() override {
        auto& r = *renderer;

        if (!r.waterEnabled || r.waterIndexCount == 0) {
            return;
        }

        auto drawableSize = r.swapchain->drawableSize();
        glm::vec2 screenSize = glm::vec2(drawableSize.width, drawableSize.height);
        auto time = (float)SDL_GetTicks() / 1000.0f;

        // Build model matrix from transform
        glm::mat4 modelMatrix = glm::mat4(1.0f);
        modelMatrix = glm::translate(modelMatrix, r.waterTransform.position);
        modelMatrix = glm::scale(modelMatrix, r.waterTransform.scale);

        // Update water data buffer
        WaterData* waterData = reinterpret_cast<WaterData*>(r.waterDataBuffers[r.currentFrameInFlight]->contents());
        *waterData = r.waterSettings;
        waterData->modelMatrix = modelMatrix;
        waterData->time = time;
        r.waterDataBuffers[r.currentFrameInFlight]->didModifyRange(
            NS::Range::Make(0, r.waterDataBuffers[r.currentFrameInFlight]->length())
        );

        // Create render pass descriptor - renders to resolved HDR target (no MSAA for water)
        auto waterPassDesc = NS::TransferPtr(MTL::RenderPassDescriptor::renderPassDescriptor());
        auto waterPassColorRT = waterPassDesc->colorAttachments()->object(0);
        waterPassColorRT->setLoadAction(MTL::LoadActionLoad);
        waterPassColorRT->setStoreAction(MTL::StoreActionStore);
        waterPassColorRT->setTexture(r.colorRT.get());

        auto waterPassDepthRT = waterPassDesc->depthAttachment();
        waterPassDepthRT->setLoadAction(MTL::LoadActionLoad);
        waterPassDepthRT->setStoreAction(MTL::StoreActionStore);
        waterPassDepthRT->setTexture(r.depthStencilRT.get());

        // Execute the pass
        auto encoder = r.currentCommandBuffer->renderCommandEncoder(waterPassDesc.get());
        encoder->setRenderPipelineState(r.waterPipeline.get());
        encoder->setCullMode(MTL::CullModeNone);// Water is double-sided
        encoder->setFrontFacingWinding(MTL::Winding::WindingCounterClockwise);
        encoder->setDepthStencilState(r.waterDepthStencilState.get());

        // Set vertex buffers
        encoder->setVertexBuffer(r.cameraDataBuffers[r.currentFrameInFlight].get(), 0, 0);
        encoder->setVertexBuffer(r.waterDataBuffers[r.currentFrameInFlight].get(), 0, 1);
        encoder->setVertexBuffer(r.waterVertexBuffer.get(), 0, 2);

        // Set fragment textures
        encoder->setFragmentTexture(r.getTexture(r.waterNormalMap1).get(), 0);
        encoder->setFragmentTexture(r.getTexture(r.waterNormalMap2).get(), 1);
        encoder->setFragmentTexture(r.colorRT.get(), 2);// HDR scene for refraction
        encoder->setFragmentTexture(r.depthStencilRT.get(), 3);// Depth for depth softening
        encoder->setFragmentTexture(r.normalRT.get(), 4);// Scene normals for SSR
        encoder->setFragmentTexture(r.environmentCubeMap.get(), 5);// Environment cube map
        encoder->setFragmentTexture(r.getTexture(r.waterFoamMap).get(), 6);
        encoder->setFragmentTexture(r.getTexture(r.waterNoiseMap).get(), 7);

        // Set fragment buffers
        encoder->setFragmentBuffer(r.waterDataBuffers[r.currentFrameInFlight].get(), 0, 0);
        encoder->setFragmentBuffer(r.cameraDataBuffers[r.currentFrameInFlight].get(), 0, 1);
        encoder->setFragmentBuffer(r.directionalLightBuffer.get(), 0, 2);
        encoder->setFragmentBytes(&screenSize, sizeof(glm::vec2), 3);

        // Draw water mesh
        encoder->drawIndexedPrimitives(
            MTL::PrimitiveType::PrimitiveTypeTriangle,
            r.waterIndexCount,
            MTL::IndexTypeUInt32,
            r.waterIndexBuffer.get(),
            0
        );
        r.drawCount++;

        encoder->endEncoding();
    }
};

// Particle pass: GPU particle simulation and rendering
class ParticlePass : public RenderPass {
public:
    explicit ParticlePass(Renderer_Metal* renderer) : RenderPass(renderer) {
    }

    const char* getName() const override {
        return "ParticlePass";
    }

    void execute() override {
        auto& r = *renderer;

        // Skip if particle system is disabled or pipelines aren't ready
        if (!r.particleSystemEnabled || r.particleCount == 0) {
            return;
        }
        if (!r.particleForcePipeline || !r.particleIntegratePipeline || !r.particleRenderPipeline) {
            return;
        }

        auto time = (float)SDL_GetTicks() / 1000.0f;
        float deltaTime = 1.0f / 60.0f;// Use fixed timestep to avoid issues

        // Compute attractor position (in front of camera)
        glm::vec3 camPos = r.currentCamera->getEye();
        glm::mat4 view = r.currentCamera->getViewMatrix();
        glm::vec3 forward = -glm::vec3(view[0][2], view[1][2], view[2][2]);
        glm::vec3 attractorPos = camPos + forward * 3.0f;

        // Update simulation params buffer
        struct ParticleSimParams {
            glm::vec2 resolution;
            glm::vec2 mousePosition;
            float time;
            float deltaTime;
            Uint32 particleCount;
            float _pad1;
        } simParams;

        auto drawableSize = r.swapchain->drawableSize();
        simParams.resolution = glm::vec2(drawableSize.width, drawableSize.height);
        simParams.mousePosition = glm::vec2(0.0f);
        simParams.time = time;
        simParams.deltaTime = deltaTime;
        simParams.particleCount = r.particleCount;

        memcpy(r.particleSimParamsBuffers[r.currentFrameInFlight]->contents(), &simParams, sizeof(ParticleSimParams));
        r.particleSimParamsBuffers[r.currentFrameInFlight]->didModifyRange(NS::Range::Make(0, sizeof(ParticleSimParams))
        );

        // Update attractor buffer
        struct ParticleAttractor {
            glm::vec3 position;
            float strength;
        } attractor;

        attractor.position = attractorPos;
        attractor.strength = 50.0f;// Increased strength

        memcpy(r.particleAttractorBuffers[r.currentFrameInFlight]->contents(), &attractor, sizeof(ParticleAttractor));
        r.particleAttractorBuffers[r.currentFrameInFlight]->didModifyRange(NS::Range::Make(0, sizeof(ParticleAttractor))
        );

        // Compute passes (single particle buffer - persistent state)
        {
            auto computeEncoder = r.currentCommandBuffer->computeCommandEncoder();

            // Force calculation
            computeEncoder->setComputePipelineState(r.particleForcePipeline.get());
            computeEncoder->setBuffer(r.particleBuffer.get(), 0, 0);
            computeEncoder->setBuffer(r.particleSimParamsBuffers[r.currentFrameInFlight].get(), 0, 1);
            computeEncoder->setBuffer(r.particleAttractorBuffers[r.currentFrameInFlight].get(), 0, 2);

            MTL::Size gridSize = MTL::Size((r.particleCount + 255) / 256, 1, 1);
            MTL::Size threadGroupSize = MTL::Size(256, 1, 1);
            computeEncoder->dispatchThreadgroups(gridSize, threadGroupSize);

            // Integration
            computeEncoder->setComputePipelineState(r.particleIntegratePipeline.get());
            computeEncoder->setBuffer(r.particleBuffer.get(), 0, 0);
            computeEncoder->setBuffer(r.particleSimParamsBuffers[r.currentFrameInFlight].get(), 0, 1);
            computeEncoder->dispatchThreadgroups(gridSize, threadGroupSize);

            computeEncoder->endEncoding();
        }

        // Render pass: Draw particles
        {
            auto renderPassDesc = NS::TransferPtr(MTL::RenderPassDescriptor::renderPassDescriptor());
            auto colorAttachment = renderPassDesc->colorAttachments()->object(0);
            colorAttachment->setLoadAction(MTL::LoadActionLoad);
            colorAttachment->setStoreAction(MTL::StoreActionStore);
            colorAttachment->setTexture(r.colorRT.get());

            auto depthAttachment = renderPassDesc->depthAttachment();
            depthAttachment->setLoadAction(MTL::LoadActionLoad);
            depthAttachment->setStoreAction(MTL::StoreActionDontCare);
            depthAttachment->setTexture(r.depthStencilRT.get());

            auto encoder = r.currentCommandBuffer->renderCommandEncoder(renderPassDesc.get());
            encoder->setRenderPipelineState(r.particleRenderPipeline.get());
            encoder->setDepthStencilState(r.particleDepthStencilState.get());
            encoder->setCullMode(MTL::CullModeNone);

            // Set buffers
            encoder->setVertexBuffer(r.cameraDataBuffers[r.currentFrameInFlight].get(), 0, 0);

            struct ParticlePushConstants {
                float particleSize;
                float _pad1;
                float _pad2;
                float _pad3;
            } pushConstants;
            pushConstants.particleSize = 0.1f;// Larger particles for visibility
            encoder->setVertexBytes(&pushConstants, sizeof(ParticlePushConstants), 1);
            encoder->setVertexBuffer(r.particleBuffer.get(), 0, 2);

            // Draw 6 vertices per particle (2 triangles = 1 quad), instanced
            encoder->drawPrimitives(MTL::PrimitiveType::PrimitiveTypeTriangle, 0, 6, r.particleCount);
            encoder->endEncoding();
        }
    }
};

// Light scattering pass: Renders volumetric god rays effect
class LightScatteringPass : public RenderPass {
public:
    explicit LightScatteringPass(Renderer_Metal* renderer) : RenderPass(renderer) {
    }

    const char* getName() const override {
        return "LightScatteringPass";
    }

    void execute() override {
        auto& r = *renderer;

        if (!r.lightScatteringEnabled) return;

        auto drawableSize = r.swapchain->drawableSize();
        glm::vec2 screenSize = glm::vec2(drawableSize.width, drawableSize.height);

        // Calculate sun screen position by projecting sun direction
        AtmosphereData* atmos = reinterpret_cast<AtmosphereData*>(r.atmosphereDataBuffer->contents());
        glm::vec3 sunDir = glm::normalize(atmos->sunDirection);

        // Project sun position to screen space
        // Sun is at infinity, so we use camera position + sun direction * large distance
        glm::vec3 camPos = r.currentCamera->getEye();
        glm::vec3 sunWorldPos = camPos + sunDir * 10000.0f;

        glm::mat4 viewProj = r.currentCamera->getProjMatrix() * r.currentCamera->getViewMatrix();
        glm::vec4 sunClip = viewProj * glm::vec4(sunWorldPos, 1.0f);

        // Check if sun is behind camera
        if (sunClip.w <= 0.0f) {
            return;// Sun behind camera, no god rays
        }

        // Convert to NDC then to UV [0,1]
        glm::vec2 sunNDC = glm::vec2(sunClip.x, sunClip.y) / sunClip.w;
        glm::vec2 sunScreenPos = sunNDC * 0.5f + 0.5f;
        sunScreenPos.y = 1.0f - sunScreenPos.y;// Flip Y for Metal

        // Update light scattering data buffer
        LightScatteringData* lsData =
            reinterpret_cast<LightScatteringData*>(r.lightScatteringDataBuffers[r.currentFrameInFlight]->contents());
        lsData->sunScreenPos = sunScreenPos;
        lsData->screenSize = screenSize;
        lsData->density = r.lightScatteringSettings.density;
        lsData->weight = r.lightScatteringSettings.weight;
        lsData->decay = r.lightScatteringSettings.decay;
        lsData->exposure = r.lightScatteringSettings.exposure;
        lsData->numSamples = r.lightScatteringSettings.numSamples;
        lsData->maxDistance = r.lightScatteringSettings.maxDistance;
        lsData->sunIntensity = r.lightScatteringSettings.sunIntensity;
        lsData->mieG = r.lightScatteringSettings.mieG;
        lsData->sunColor = atmos->sunColor;
        lsData->depthThreshold = r.lightScatteringSettings.depthThreshold;
        lsData->jitter = r.lightScatteringSettings.jitter;
        r.lightScatteringDataBuffers[r.currentFrameInFlight]->didModifyRange(
            NS::Range::Make(0, r.lightScatteringDataBuffers[r.currentFrameInFlight]->length())
        );

        // Create render pass descriptor - render to light scattering RT
        auto lsPassDesc = NS::TransferPtr(MTL::RenderPassDescriptor::renderPassDescriptor());
        auto lsPassColorRT = lsPassDesc->colorAttachments()->object(0);
        lsPassColorRT->setClearColor(MTL::ClearColor(0.0, 0.0, 0.0, 0.0));
        lsPassColorRT->setLoadAction(MTL::LoadActionClear);
        lsPassColorRT->setStoreAction(MTL::StoreActionStore);
        lsPassColorRT->setTexture(r.lightScatteringRT.get());

        // Execute the pass
        auto encoder = r.currentCommandBuffer->renderCommandEncoder(lsPassDesc.get());
        encoder->setRenderPipelineState(r.lightScatteringPipeline.get());
        encoder->setCullMode(MTL::CullModeNone);

        // Set textures
        encoder->setFragmentTexture(r.colorRT.get(), 0);// Scene color
        encoder->setFragmentTexture(r.depthStencilRT.get(), 1);// Scene depth

        // Set buffers
        encoder->setFragmentBuffer(r.lightScatteringDataBuffers[r.currentFrameInFlight].get(), 0, 0);
        encoder->setFragmentBuffer(r.frameDataBuffers[r.currentFrameInFlight].get(), 0, 1);

        // Draw full-screen triangle
        encoder->drawPrimitives(MTL::PrimitiveType::PrimitiveTypeTriangle, 0, 3, 1);
        encoder->endEncoding();
    }
};

// ============================================================================
// Volumetric Fog Pass: Height-based fog with scattering
// ============================================================================

class VolumetricFogPass : public RenderPass {
public:
    explicit VolumetricFogPass(Renderer_Metal* renderer) : RenderPass(renderer) {
    }

    const char* getName() const override {
        return "VolumetricFogPass";
    }

    void execute() override {
        auto& r = *renderer;

        if (!r.volumetricFogEnabled || !r.fogSimplePipeline) return;

        auto drawableSize = r.swapchain->drawableSize();

        // Update fog data buffer
        AtmosphereData* atmos = reinterpret_cast<AtmosphereData*>(r.atmosphereDataBuffer->contents());

        VolumetricFogData* fogData =
            reinterpret_cast<VolumetricFogData*>(r.volumetricFogDataBuffers[r.currentFrameInFlight]->contents());
        fogData->invViewProj = glm::inverse(r.currentCamera->getProjMatrix() * r.currentCamera->getViewMatrix());
        fogData->cameraPosition = r.currentCamera->getEye();
        fogData->sunDirection = glm::normalize(atmos->sunDirection);
        fogData->sunColor = atmos->sunColor;
        fogData->sunIntensity = atmos->sunIntensity;
        fogData->screenSize = glm::vec2(drawableSize.width, drawableSize.height);
        fogData->nearPlane = r.currentCamera->near();
        fogData->farPlane = r.volumetricFogSettings.farPlane;
        fogData->frameIndex = r.currentFrameInFlight;
        fogData->time = r.volumetricFogSettings.time;

        // Copy settings
        fogData->fogDensity = r.volumetricFogSettings.fogDensity;
        fogData->fogHeightFalloff = r.volumetricFogSettings.fogHeightFalloff;
        fogData->fogBaseHeight = r.volumetricFogSettings.fogBaseHeight;
        fogData->fogMaxHeight = r.volumetricFogSettings.fogMaxHeight;
        fogData->scatteringCoeff = r.volumetricFogSettings.scatteringCoeff;
        fogData->extinctionCoeff = r.volumetricFogSettings.extinctionCoeff;
        fogData->anisotropy = r.volumetricFogSettings.anisotropy;
        fogData->ambientIntensity = r.volumetricFogSettings.ambientIntensity;
        fogData->noiseScale = r.volumetricFogSettings.noiseScale;
        fogData->noiseIntensity = r.volumetricFogSettings.noiseIntensity;
        fogData->windSpeed = r.volumetricFogSettings.windSpeed;
        fogData->windDirection = r.volumetricFogSettings.windDirection;
        fogData->temporalBlend = r.volumetricFogSettings.temporalBlend;

        r.volumetricFogDataBuffers[r.currentFrameInFlight]->didModifyRange(
            NS::Range::Make(0, r.volumetricFogDataBuffers[r.currentFrameInFlight]->length())
        );

        // Simple fog pass - ping-pong: read from colorRT, write to tempColorRT
        auto passDesc = NS::TransferPtr(MTL::RenderPassDescriptor::renderPassDescriptor());
        auto colorAttach = passDesc->colorAttachments()->object(0);
        colorAttach->setLoadAction(MTL::LoadActionDontCare);
        colorAttach->setStoreAction(MTL::StoreActionStore);
        colorAttach->setTexture(r.tempColorRT.get());// Write to temp

        auto encoder = r.currentCommandBuffer->renderCommandEncoder(passDesc.get());
        encoder->setRenderPipelineState(r.fogSimplePipeline.get());
        encoder->setCullMode(MTL::CullModeNone);
        encoder->setFragmentTexture(r.colorRT.get(), 0);// Read from color
        encoder->setFragmentTexture(r.depthStencilRT.get(), 1);
        encoder->setFragmentBuffer(r.volumetricFogDataBuffers[r.currentFrameInFlight].get(), 0, 0);
        encoder->setFragmentBuffer(r.cameraDataBuffers[r.currentFrameInFlight].get(), 0, 1);
        encoder->drawPrimitives(MTL::PrimitiveType::PrimitiveTypeTriangle, 0, 3, 1);
        encoder->endEncoding();

        // Swap so colorRT now contains the fogged result
        std::swap(r.colorRT, r.tempColorRT);
    }
};

// ============================================================================
// Volumetric Cloud Pass: Ray-marched clouds
// ============================================================================

class VolumetricCloudPass : public RenderPass {
public:
    explicit VolumetricCloudPass(Renderer_Metal* renderer) : RenderPass(renderer) {
    }

    const char* getName() const override {
        return "VolumetricCloudPass";
    }

    void execute() override {
        auto& r = *renderer;

        // Check if any required pipeline is available
        bool hasLowResPipeline = r.cloudLowResPipeline && r.cloudCompositePipeline;
        bool hasFullResPipeline = r.cloudRenderPipeline.get() != nullptr;

        if (!r.volumetricCloudsEnabled || (!hasLowResPipeline && !hasFullResPipeline)) return;

        auto drawableSize = r.swapchain->drawableSize();

        // Update cloud data buffer
        AtmosphereData* atmos = reinterpret_cast<AtmosphereData*>(r.atmosphereDataBuffer->contents());

        VolumetricCloudData* cloudData =
            reinterpret_cast<VolumetricCloudData*>(r.volumetricCloudDataBuffers[r.currentFrameInFlight]->contents());
        cloudData->invViewProj = glm::inverse(r.currentCamera->getProjMatrix() * r.currentCamera->getViewMatrix());
        cloudData->prevViewProj = r.volumetricCloudSettings.prevViewProj;// For temporal reprojection
        cloudData->cameraPosition = r.currentCamera->getEye();
        cloudData->sunDirection = glm::normalize(atmos->sunDirection);
        cloudData->sunColor = atmos->sunColor;
        cloudData->sunIntensity = atmos->sunIntensity;
        cloudData->frameIndex = r.currentFrameInFlight;
        cloudData->time = r.volumetricCloudSettings.time;

        // Update wind offset (accumulate over time)
        r.volumetricCloudSettings.windOffset +=
            r.volumetricCloudSettings.windDirection * r.volumetricCloudSettings.windSpeed * 0.016f;
        cloudData->windOffset = r.volumetricCloudSettings.windOffset;

        // Copy settings
        cloudData->cloudLayerBottom = r.volumetricCloudSettings.cloudLayerBottom;
        cloudData->cloudLayerTop = r.volumetricCloudSettings.cloudLayerTop;
        cloudData->cloudLayerThickness = cloudData->cloudLayerTop - cloudData->cloudLayerBottom;
        cloudData->cloudCoverage = r.volumetricCloudSettings.cloudCoverage;
        cloudData->cloudDensity = r.volumetricCloudSettings.cloudDensity;
        cloudData->cloudType = r.volumetricCloudSettings.cloudType;
        cloudData->erosionStrength = r.volumetricCloudSettings.erosionStrength;
        cloudData->shapeNoiseScale = r.volumetricCloudSettings.shapeNoiseScale;
        cloudData->detailNoiseScale = r.volumetricCloudSettings.detailNoiseScale;
        cloudData->ambientIntensity = r.volumetricCloudSettings.ambientIntensity;
        cloudData->silverLiningIntensity = r.volumetricCloudSettings.silverLiningIntensity;
        cloudData->silverLiningSpread = r.volumetricCloudSettings.silverLiningSpread;
        cloudData->phaseG1 = r.volumetricCloudSettings.phaseG1;
        cloudData->phaseG2 = r.volumetricCloudSettings.phaseG2;
        cloudData->phaseBlend = r.volumetricCloudSettings.phaseBlend;
        cloudData->powderStrength = r.volumetricCloudSettings.powderStrength;
        cloudData->windDirection = r.volumetricCloudSettings.windDirection;
        cloudData->windSpeed = r.volumetricCloudSettings.windSpeed;
        cloudData->primarySteps = r.volumetricCloudSettings.primarySteps;
        cloudData->lightSteps = r.volumetricCloudSettings.lightSteps;
        cloudData->temporalBlend = r.volumetricCloudSettings.temporalBlend;

        r.volumetricCloudDataBuffers[r.currentFrameInFlight]->didModifyRange(
            NS::Range::Make(0, r.volumetricCloudDataBuffers[r.currentFrameInFlight]->length())
        );

        // Use quarter-resolution pipeline if available, otherwise fall back to full-res
        if (hasLowResPipeline && r.cloudRT && r.cloudHistoryRT) {
            Uint32 cloudWidth = drawableSize.width / 4;
            Uint32 cloudHeight = drawableSize.height / 4;

            // Update screen size for quarter resolution
            cloudData->screenSize = glm::vec2(cloudWidth, cloudHeight);
            r.volumetricCloudDataBuffers[r.currentFrameInFlight]->didModifyRange(
                NS::Range::Make(0, r.volumetricCloudDataBuffers[r.currentFrameInFlight]->length())
            );

            // ================================================================
            // Pass 1: Render clouds at quarter resolution
            // ================================================================
            {
                auto passDesc = NS::TransferPtr(MTL::RenderPassDescriptor::renderPassDescriptor());
                auto colorAttach = passDesc->colorAttachments()->object(0);
                colorAttach->setClearColor(MTL::ClearColor(0.0, 0.0, 0.0, 1.0));
                colorAttach->setLoadAction(MTL::LoadActionClear);
                colorAttach->setStoreAction(MTL::StoreActionStore);
                colorAttach->setTexture(r.cloudRT.get());

                auto encoder = r.currentCommandBuffer->renderCommandEncoder(passDesc.get());
                encoder->setRenderPipelineState(r.cloudLowResPipeline.get());
                encoder->setCullMode(MTL::CullModeNone);

                // Set viewport to quarter resolution
                MTL::Viewport viewport;
                viewport.originX = 0.0;
                viewport.originY = 0.0;
                viewport.width = cloudWidth;
                viewport.height = cloudHeight;
                viewport.znear = 0.0;
                viewport.zfar = 1.0;
                encoder->setViewport(viewport);

                encoder->setFragmentTexture(r.depthStencilRT.get(), 0);
                encoder->setFragmentBuffer(r.volumetricCloudDataBuffers[r.currentFrameInFlight].get(), 0, 0);
                encoder->setFragmentBuffer(r.cameraDataBuffers[r.currentFrameInFlight].get(), 0, 1);
                encoder->drawPrimitives(MTL::PrimitiveType::PrimitiveTypeTriangle, 0, 3, 1);
                encoder->endEncoding();
            }

            // ================================================================
            // Pass 2: Temporal resolve (blend current with history)
            // ================================================================
            if (r.cloudTemporalResolvePipeline) {
                // Swap cloudRT and cloudHistoryRT for temporal accumulation
                auto passDesc = NS::TransferPtr(MTL::RenderPassDescriptor::renderPassDescriptor());
                auto colorAttach = passDesc->colorAttachments()->object(0);
                colorAttach->setLoadAction(MTL::LoadActionDontCare);
                colorAttach->setStoreAction(MTL::StoreActionStore);
                colorAttach->setTexture(r.cloudHistoryRT.get());

                auto encoder = r.currentCommandBuffer->renderCommandEncoder(passDesc.get());
                encoder->setRenderPipelineState(r.cloudTemporalResolvePipeline.get());
                encoder->setCullMode(MTL::CullModeNone);

                MTL::Viewport viewport;
                viewport.originX = 0.0;
                viewport.originY = 0.0;
                viewport.width = cloudWidth;
                viewport.height = cloudHeight;
                viewport.znear = 0.0;
                viewport.zfar = 1.0;
                encoder->setViewport(viewport);

                encoder->setFragmentTexture(r.cloudRT.get(), 0);// Current frame
                encoder->setFragmentTexture(r.cloudHistoryRT.get(), 1);// History (will be overwritten)
                encoder->setFragmentTexture(r.depthStencilRT.get(), 2);
                encoder->setFragmentBuffer(r.volumetricCloudDataBuffers[r.currentFrameInFlight].get(), 0, 0);
                encoder->drawPrimitives(MTL::PrimitiveType::PrimitiveTypeTriangle, 0, 3, 1);
                encoder->endEncoding();

                // Swap RT pointers for next frame
                std::swap(r.cloudRT, r.cloudHistoryRT);
            }

            // ================================================================
            // Pass 3: Upscale and composite - ping-pong to avoid hazard
            // ================================================================
            {
                // Restore screen size for composite pass
                cloudData->screenSize = glm::vec2(drawableSize.width, drawableSize.height);
                r.volumetricCloudDataBuffers[r.currentFrameInFlight]->didModifyRange(
                    NS::Range::Make(0, r.volumetricCloudDataBuffers[r.currentFrameInFlight]->length())
                );

                auto passDesc = NS::TransferPtr(MTL::RenderPassDescriptor::renderPassDescriptor());
                auto colorAttach = passDesc->colorAttachments()->object(0);
                colorAttach->setLoadAction(MTL::LoadActionDontCare);
                colorAttach->setStoreAction(MTL::StoreActionStore);
                colorAttach->setTexture(r.tempColorRT.get());// Write to temp

                auto encoder = r.currentCommandBuffer->renderCommandEncoder(passDesc.get());
                encoder->setRenderPipelineState(r.cloudCompositePipeline.get());
                encoder->setCullMode(MTL::CullModeNone);
                encoder->setFragmentTexture(r.colorRT.get(), 0);// Read from color
                encoder->setFragmentTexture(r.cloudRT.get(), 1);// Cloud (quarter res)
                encoder->setFragmentTexture(r.depthStencilRT.get(), 2);
                encoder->setFragmentBuffer(r.volumetricCloudDataBuffers[r.currentFrameInFlight].get(), 0, 0);
                encoder->drawPrimitives(MTL::PrimitiveType::PrimitiveTypeTriangle, 0, 3, 1);
                encoder->endEncoding();

                // Swap so colorRT now contains the composited result
                std::swap(r.colorRT, r.tempColorRT);
            }
        } else if (hasFullResPipeline) {
            // Fallback: Full resolution rendering - ping-pong to avoid hazard
            cloudData->screenSize = glm::vec2(drawableSize.width, drawableSize.height);
            r.volumetricCloudDataBuffers[r.currentFrameInFlight]->didModifyRange(
                NS::Range::Make(0, r.volumetricCloudDataBuffers[r.currentFrameInFlight]->length())
            );

            auto passDesc = NS::TransferPtr(MTL::RenderPassDescriptor::renderPassDescriptor());
            auto colorAttach = passDesc->colorAttachments()->object(0);
            colorAttach->setLoadAction(MTL::LoadActionDontCare);
            colorAttach->setStoreAction(MTL::StoreActionStore);
            colorAttach->setTexture(r.tempColorRT.get());// Write to temp

            auto encoder = r.currentCommandBuffer->renderCommandEncoder(passDesc.get());
            encoder->setRenderPipelineState(r.cloudRenderPipeline.get());
            encoder->setCullMode(MTL::CullModeNone);
            encoder->setFragmentTexture(r.colorRT.get(), 0);// Read from color
            encoder->setFragmentTexture(r.depthStencilRT.get(), 1);
            encoder->setFragmentBuffer(r.volumetricCloudDataBuffers[r.currentFrameInFlight].get(), 0, 0);
            encoder->setFragmentBuffer(r.cameraDataBuffers[r.currentFrameInFlight].get(), 0, 1);
            encoder->drawPrimitives(MTL::PrimitiveType::PrimitiveTypeTriangle, 0, 3, 1);
            encoder->endEncoding();

            // Swap so colorRT now contains the composited result
            std::swap(r.colorRT, r.tempColorRT);
        }

        // Store current view-proj for next frame's temporal reprojection
        r.volumetricCloudSettings.prevViewProj = r.currentCamera->getProjMatrix() * r.currentCamera->getViewMatrix();
    }
};

// ============================================================================
// Sun Flare Pass: Lens flare effect with procedural textures
// ============================================================================

class SunFlarePass : public RenderPass {
public:
    explicit SunFlarePass(Renderer_Metal* renderer) : RenderPass(renderer) {
    }

    const char* getName() const override {
        return "SunFlarePass";
    }

    void execute() override {
        auto& r = *renderer;

        if (!r.sunFlareEnabled || !r.sunFlarePipeline) return;

        auto drawableSize = r.swapchain->drawableSize();
        glm::vec2 screenSize = glm::vec2(drawableSize.width, drawableSize.height);

        // Calculate sun screen position
        AtmosphereData* atmos = reinterpret_cast<AtmosphereData*>(r.atmosphereDataBuffer->contents());
        glm::vec3 sunDir = glm::normalize(atmos->sunDirection);
        glm::vec3 camPos = r.currentCamera->getEye();
        glm::vec3 sunWorldPos = camPos + sunDir * 10000.0f;

        glm::mat4 viewProj = r.currentCamera->getProjMatrix() * r.currentCamera->getViewMatrix();
        glm::vec4 sunClip = viewProj * glm::vec4(sunWorldPos, 1.0f);

        // Sun behind camera
        if (sunClip.w <= 0.0f) {
            return;
        }

        // Convert to screen UV
        glm::vec2 sunNDC = glm::vec2(sunClip.x, sunClip.y) / sunClip.w;
        glm::vec2 sunScreenPos = sunNDC * 0.5f + 0.5f;
        sunScreenPos.y = 1.0f - sunScreenPos.y;

        // Update flare data buffer
        SunFlareData* flareData =
            reinterpret_cast<SunFlareData*>(r.sunFlareDataBuffers[r.currentFrameInFlight]->contents());
        flareData->sunScreenPos = sunScreenPos;
        flareData->screenSize = screenSize;
        flareData->screenCenter = glm::vec2(0.5f, 0.5f);
        flareData->aspectRatio = glm::vec2(screenSize.x / screenSize.y, 1.0f);
        flareData->sunColor = atmos->sunColor;

        // Simple visibility check using depth at sun position
        // For proper occlusion, we'd use the compute shader, but this is a simple approximation
        float visibility = 1.0f;
        if (sunScreenPos.x < 0.0f || sunScreenPos.x > 1.0f || sunScreenPos.y < 0.0f || sunScreenPos.y > 1.0f) {
            visibility = 0.0f;
        }
        flareData->visibility = visibility;

        // Copy settings
        flareData->sunIntensity = r.sunFlareSettings.sunIntensity;
        flareData->fadeEdge = r.sunFlareSettings.fadeEdge;
        flareData->glowIntensity = r.sunFlareSettings.glowIntensity;
        flareData->glowFalloff = r.sunFlareSettings.glowFalloff;
        flareData->glowSize = r.sunFlareSettings.glowSize;
        flareData->haloIntensity = r.sunFlareSettings.haloIntensity;
        flareData->haloRadius = r.sunFlareSettings.haloRadius;
        flareData->haloWidth = r.sunFlareSettings.haloWidth;
        flareData->haloFalloff = r.sunFlareSettings.haloFalloff;
        flareData->ghostCount = r.sunFlareSettings.ghostCount;
        flareData->ghostSpacing = r.sunFlareSettings.ghostSpacing;
        flareData->ghostIntensity = r.sunFlareSettings.ghostIntensity;
        flareData->ghostSize = r.sunFlareSettings.ghostSize;
        flareData->ghostChromaticOffset = r.sunFlareSettings.ghostChromaticOffset;
        flareData->ghostFalloff = r.sunFlareSettings.ghostFalloff;
        flareData->streakIntensity = r.sunFlareSettings.streakIntensity;
        flareData->streakLength = r.sunFlareSettings.streakLength;
        flareData->streakFalloff = r.sunFlareSettings.streakFalloff;
        flareData->starburstIntensity = r.sunFlareSettings.starburstIntensity;
        flareData->starburstSize = r.sunFlareSettings.starburstSize;
        flareData->starburstPoints = r.sunFlareSettings.starburstPoints;
        flareData->starburstRotation = r.sunFlareSettings.starburstRotation;
        flareData->dirtIntensity = r.sunFlareSettings.dirtIntensity;
        flareData->dirtScale = r.sunFlareSettings.dirtScale;
        flareData->time = r.sunFlareSettings.time;

        r.sunFlareDataBuffers[r.currentFrameInFlight]->didModifyRange(
            NS::Range::Make(0, r.sunFlareDataBuffers[r.currentFrameInFlight]->length())
        );

        // Render flare with additive blending (hardware blends output onto existing content)
        auto passDesc = NS::TransferPtr(MTL::RenderPassDescriptor::renderPassDescriptor());
        auto colorAttach = passDesc->colorAttachments()->object(0);
        colorAttach->setLoadAction(MTL::LoadActionLoad);// Preserve existing bloom result
        colorAttach->setStoreAction(MTL::StoreActionStore);
        colorAttach->setTexture(r.bloomResultRT.get());

        auto encoder = r.currentCommandBuffer->renderCommandEncoder(passDesc.get());
        encoder->setRenderPipelineState(r.sunFlarePipeline.get());
        encoder->setCullMode(MTL::CullModeNone);
        // No need to bind bloomResultRT as input - hardware blending handles compositing
        encoder->setFragmentTexture(r.depthStencilRT.get(), 1);
        encoder->setFragmentBuffer(r.sunFlareDataBuffers[r.currentFrameInFlight].get(), 0, 0);
        encoder->drawPrimitives(MTL::PrimitiveType::PrimitiveTypeTriangle, 0, 3, 1);
        encoder->endEncoding();
    }
};

// ============================================================================
// Bloom passes: Physically-based bloom implementation
// ============================================================================

// Bloom brightness pass: Extracts bright pixels from the scene
class BloomBrightnessPass : public RenderPass {
public:
    explicit BloomBrightnessPass(Renderer_Metal* renderer) : RenderPass(renderer) {
    }

    const char* getName() const override {
        return "BloomBrightnessPass";
    }

    void execute() override {
        auto& r = *renderer;
        auto passDesc = NS::TransferPtr(MTL::RenderPassDescriptor::renderPassDescriptor());
        auto colorRT = passDesc->colorAttachments()->object(0);
        colorRT->setClearColor(MTL::ClearColor(0.0, 0.0, 0.0, 1.0));
        colorRT->setLoadAction(MTL::LoadActionClear);
        colorRT->setStoreAction(MTL::StoreActionStore);
        colorRT->setTexture(r.bloomBrightnessRT.get());

        auto encoder = r.currentCommandBuffer->renderCommandEncoder(passDesc.get());
        encoder->setRenderPipelineState(r.bloomBrightnessPipeline.get());
        encoder->setCullMode(MTL::CullModeBack);
        encoder->setFrontFacingWinding(MTL::Winding::WindingCounterClockwise);
        encoder->setFragmentTexture(r.colorRT.get(), 0);
        encoder->setFragmentBytes(&r.bloomThreshold, sizeof(float), 0);
        encoder->drawPrimitives(MTL::PrimitiveType::PrimitiveTypeTriangle, 0, 3, 1);
        encoder->endEncoding();
    }
};

// Bloom downsample pass: Creates the bloom mipmap pyramid
class BloomDownsamplePass : public RenderPass {
public:
    explicit BloomDownsamplePass(Renderer_Metal* renderer) : RenderPass(renderer) {
    }

    const char* getName() const override {
        return "BloomDownsamplePass";
    }

    void execute() override {
        auto& r = *renderer;

        // First downsample from brightness RT to pyramid level 0
        {
            auto passDesc = NS::TransferPtr(MTL::RenderPassDescriptor::renderPassDescriptor());
            auto colorRT = passDesc->colorAttachments()->object(0);
            colorRT->setClearColor(MTL::ClearColor(0.0, 0.0, 0.0, 1.0));
            colorRT->setLoadAction(MTL::LoadActionClear);
            colorRT->setStoreAction(MTL::StoreActionStore);
            colorRT->setTexture(r.bloomPyramidRTs[0].get());

            auto encoder = r.currentCommandBuffer->renderCommandEncoder(passDesc.get());
            encoder->setRenderPipelineState(r.bloomDownsamplePipeline.get());
            encoder->setCullMode(MTL::CullModeBack);
            encoder->setFrontFacingWinding(MTL::Winding::WindingCounterClockwise);
            encoder->setFragmentTexture(r.bloomBrightnessRT.get(), 0);
            encoder->drawPrimitives(MTL::PrimitiveType::PrimitiveTypeTriangle, 0, 3, 1);
            encoder->endEncoding();
        }

        // Downsample through the rest of the pyramid
        for (Uint32 i = 1; i < r.BLOOM_PYRAMID_LEVELS; i++) {
            auto passDesc = NS::TransferPtr(MTL::RenderPassDescriptor::renderPassDescriptor());
            auto colorRT = passDesc->colorAttachments()->object(0);
            colorRT->setClearColor(MTL::ClearColor(0.0, 0.0, 0.0, 1.0));
            colorRT->setLoadAction(MTL::LoadActionClear);
            colorRT->setStoreAction(MTL::StoreActionStore);
            colorRT->setTexture(r.bloomPyramidRTs[i].get());

            auto encoder = r.currentCommandBuffer->renderCommandEncoder(passDesc.get());
            encoder->setRenderPipelineState(r.bloomDownsamplePipeline.get());
            encoder->setCullMode(MTL::CullModeBack);
            encoder->setFrontFacingWinding(MTL::Winding::WindingCounterClockwise);
            encoder->setFragmentTexture(r.bloomPyramidRTs[i - 1].get(), 0);
            encoder->drawPrimitives(MTL::PrimitiveType::PrimitiveTypeTriangle, 0, 3, 1);
            encoder->endEncoding();
        }
    }
};

// Bloom upsample pass: Upsamples and accumulates the bloom
class BloomUpsamplePass : public RenderPass {
public:
    explicit BloomUpsamplePass(Renderer_Metal* renderer) : RenderPass(renderer) {
    }

    const char* getName() const override {
        return "BloomUpsamplePass";
    }

    void execute() override {
        auto& r = *renderer;

        // Upsample from bottom of pyramid to top, accumulating bloom
        for (int i = static_cast<int>(r.BLOOM_PYRAMID_LEVELS) - 2; i >= 0; i--) {
            auto passDesc = NS::TransferPtr(MTL::RenderPassDescriptor::renderPassDescriptor());
            auto colorRT = passDesc->colorAttachments()->object(0);
            colorRT->setLoadAction(MTL::LoadActionLoad);// Load to blend with existing content
            colorRT->setStoreAction(MTL::StoreActionStore);
            colorRT->setTexture(r.bloomPyramidRTs[i].get());

            auto encoder = r.currentCommandBuffer->renderCommandEncoder(passDesc.get());
            encoder->setRenderPipelineState(r.bloomUpsamplePipeline.get());
            encoder->setCullMode(MTL::CullModeBack);
            encoder->setFrontFacingWinding(MTL::Winding::WindingCounterClockwise);
            encoder->setFragmentTexture(r.bloomPyramidRTs[i + 1].get(), 0);// Lower res texture
            encoder->setFragmentTexture(r.bloomPyramidRTs[i].get(), 1);// Current level to blend
            encoder->drawPrimitives(MTL::PrimitiveType::PrimitiveTypeTriangle, 0, 3, 1);
            encoder->endEncoding();
        }
    }
};

// Bloom composite pass: Combines bloom with the scene
class BloomCompositePass : public RenderPass {
public:
    explicit BloomCompositePass(Renderer_Metal* renderer) : RenderPass(renderer) {
    }

    const char* getName() const override {
        return "BloomCompositePass";
    }

    void execute() override {
        auto& r = *renderer;

        auto passDesc = NS::TransferPtr(MTL::RenderPassDescriptor::renderPassDescriptor());
        auto colorRT = passDesc->colorAttachments()->object(0);
        colorRT->setClearColor(MTL::ClearColor(0.0, 0.0, 0.0, 1.0));
        colorRT->setLoadAction(MTL::LoadActionClear);
        colorRT->setStoreAction(MTL::StoreActionStore);
        colorRT->setTexture(r.bloomResultRT.get());

        auto encoder = r.currentCommandBuffer->renderCommandEncoder(passDesc.get());
        encoder->setRenderPipelineState(r.bloomCompositePipeline.get());
        encoder->setCullMode(MTL::CullModeBack);
        encoder->setFrontFacingWinding(MTL::Winding::WindingCounterClockwise);
        encoder->setFragmentTexture(r.colorRT.get(), 0);// Original scene
        encoder->setFragmentTexture(r.bloomPyramidRTs[0].get(), 1);// Accumulated bloom
        encoder->setFragmentBytes(&r.bloomStrength, sizeof(float), 0);
        encoder->drawPrimitives(MTL::PrimitiveType::PrimitiveTypeTriangle, 0, 3, 1);
        encoder->endEncoding();
    }
};

// ============================================================================
// DOF (Tilt-Shift) passes: Octopath Traveler style depth of field
// ============================================================================

// DOF CoC pass: Calculate Circle of Confusion based on screen position
class DOFCoCPass : public RenderPass {
public:
    explicit DOFCoCPass(Renderer_Metal* renderer) : RenderPass(renderer) {
    }

    const char* getName() const override {
        return "DOFCoCPass";
    }

    void execute() override {
        auto& r = *renderer;

        // GPU-compatible DOF params struct (matches shader)
        struct GPUDOFParams {
            float focusCenter;
            float focusWidth;
            float focusFalloff;
            float maxBlur;
            float tiltAngle;
            float bokehRoundness;
            float padding1;
            float padding2;
        } gpuParams = { r.dofParams.focusCenter,
                        r.dofParams.focusWidth,
                        r.dofParams.focusFalloff,
                        r.dofParams.maxBlur,
                        r.dofParams.tiltAngle,
                        r.dofParams.bokehRoundness,
                        0.0f,
                        0.0f };

        auto passDesc = NS::TransferPtr(MTL::RenderPassDescriptor::renderPassDescriptor());
        auto colorRT = passDesc->colorAttachments()->object(0);
        colorRT->setClearColor(MTL::ClearColor(0.0, 0.0, 0.0, 0.0));
        colorRT->setLoadAction(MTL::LoadActionClear);
        colorRT->setStoreAction(MTL::StoreActionStore);
        colorRT->setTexture(r.dofCoCRT.get());

        auto encoder = r.currentCommandBuffer->renderCommandEncoder(passDesc.get());
        encoder->setRenderPipelineState(r.dofCoCPipeline.get());
        encoder->setCullMode(MTL::CullModeBack);
        encoder->setFrontFacingWinding(MTL::Winding::WindingCounterClockwise);
        encoder->setFragmentTexture(r.bloomResultRT.get(), 0);// Input from bloom
        encoder->setFragmentTexture(r.depthStencilRT.get(), 1);// Depth (optional for hybrid mode)
        encoder->setFragmentBytes(&gpuParams, sizeof(GPUDOFParams), 0);
        encoder->drawPrimitives(MTL::PrimitiveType::PrimitiveTypeTriangle, 0, 3, 1);
        encoder->endEncoding();
    }
};

// DOF Blur pass: Apply bokeh blur based on CoC
class DOFBlurPass : public RenderPass {
public:
    explicit DOFBlurPass(Renderer_Metal* renderer) : RenderPass(renderer) {
    }

    const char* getName() const override {
        return "DOFBlurPass";
    }

    void execute() override {
        auto& r = *renderer;

        struct DOFBlurParams {
            float texelSizeX;
            float texelSizeY;
            float blurScale;
            int sampleCount;
        } blurParams = { 1.0f / r.dofBlurRT->width(), 1.0f / r.dofBlurRT->height(), 1.0f, r.dofParams.sampleCount };

        auto passDesc = NS::TransferPtr(MTL::RenderPassDescriptor::renderPassDescriptor());
        auto colorRT = passDesc->colorAttachments()->object(0);
        colorRT->setClearColor(MTL::ClearColor(0.0, 0.0, 0.0, 0.0));
        colorRT->setLoadAction(MTL::LoadActionClear);
        colorRT->setStoreAction(MTL::StoreActionStore);
        colorRT->setTexture(r.dofBlurRT.get());

        auto encoder = r.currentCommandBuffer->renderCommandEncoder(passDesc.get());
        encoder->setRenderPipelineState(r.dofBlurPipeline.get());
        encoder->setCullMode(MTL::CullModeBack);
        encoder->setFrontFacingWinding(MTL::Winding::WindingCounterClockwise);
        encoder->setFragmentTexture(r.dofCoCRT.get(), 0);
        encoder->setFragmentBytes(&blurParams, sizeof(DOFBlurParams), 0);
        encoder->drawPrimitives(MTL::PrimitiveType::PrimitiveTypeTriangle, 0, 3, 1);
        encoder->endEncoding();
    }
};

// DOF Composite pass: Blend sharp and blurred images
class DOFCompositePass : public RenderPass {
public:
    explicit DOFCompositePass(Renderer_Metal* renderer) : RenderPass(renderer) {
    }

    const char* getName() const override {
        return "DOFCompositePass";
    }

    void execute() override {
        auto& r = *renderer;

        auto passDesc = NS::TransferPtr(MTL::RenderPassDescriptor::renderPassDescriptor());
        auto colorRT = passDesc->colorAttachments()->object(0);
        colorRT->setClearColor(MTL::ClearColor(0.0, 0.0, 0.0, 1.0));
        colorRT->setLoadAction(MTL::LoadActionClear);
        colorRT->setStoreAction(MTL::StoreActionStore);
        colorRT->setTexture(r.dofResultRT.get());

        auto encoder = r.currentCommandBuffer->renderCommandEncoder(passDesc.get());
        encoder->setRenderPipelineState(r.dofCompositePipeline.get());
        encoder->setCullMode(MTL::CullModeBack);
        encoder->setFrontFacingWinding(MTL::Winding::WindingCounterClockwise);
        encoder->setFragmentTexture(r.bloomResultRT.get(), 0);// Sharp (from bloom)
        encoder->setFragmentTexture(r.dofBlurRT.get(), 1);// Blurred
        encoder->setFragmentBytes(&r.dofParams.blendSharpness, sizeof(float), 0);
        encoder->drawPrimitives(MTL::PrimitiveType::PrimitiveTypeTriangle, 0, 3, 1);
        encoder->endEncoding();
    }
};

// Post-process pass: Applies tone mapping, color grading, chromatic aberration, vignette
class PostProcessPass : public RenderPass {
public:
    explicit PostProcessPass(Renderer_Metal* renderer) : RenderPass(renderer) {
    }

    const char* getName() const override {
        return "PostProcessPass";
    }

    void execute() override {
        auto& r = *renderer;

        // GPU-compatible post-process params struct (must match shader)
        struct GPUPostProcessParams {
            float chromaticAberrationStrength;
            float chromaticAberrationFalloff;
            float vignetteStrength;
            float vignetteRadius;
            float vignetteSoftness;
            float saturation;
            float contrast;
            float brightness;
            float temperature;
            float tint;
            float exposure;
        } gpuParams = { r.postProcessParams.chromaticAberrationStrength,
                        r.postProcessParams.chromaticAberrationFalloff,
                        r.postProcessParams.vignetteStrength,
                        r.postProcessParams.vignetteRadius,
                        r.postProcessParams.vignetteSoftness,
                        r.postProcessParams.saturation,
                        r.postProcessParams.contrast,
                        r.postProcessParams.brightness,
                        r.postProcessParams.temperature,
                        r.postProcessParams.tint,
                        r.postProcessParams.exposure };

        // Create render pass descriptor
        auto postPassDesc = NS::TransferPtr(MTL::RenderPassDescriptor::renderPassDescriptor());
        auto postPassColorRT = postPassDesc->colorAttachments()->object(0);
        postPassColorRT->setClearColor(MTL::ClearColor(r.clearColor.r, r.clearColor.g, r.clearColor.b, r.clearColor.a));
        postPassColorRT->setLoadAction(MTL::LoadActionClear);
        postPassColorRT->setStoreAction(MTL::StoreActionStore);
        postPassColorRT->setTexture(r.currentDrawable->texture());

        // Execute the pass
        auto encoder = r.currentCommandBuffer->renderCommandEncoder(postPassDesc.get());
        encoder->setRenderPipelineState(r.postProcessPipeline.get());
        encoder->setCullMode(MTL::CullModeBack);
        encoder->setFrontFacingWinding(MTL::Winding::WindingCounterClockwise);

        // Input texture: DOF result if DOF enabled, otherwise bloom result
        // Note: When DOF passes are commented out, dofResultRT won't have valid content
        // So we use bloomResultRT by default. Uncomment DOF passes and change this to dofResultRT.
        encoder->setFragmentTexture(r.bloomResultRT.get(), 0);
        encoder->setFragmentTexture(r.aoRT.get(), 1);
        encoder->setFragmentTexture(r.normalRT.get(), 2);
        encoder->setFragmentTexture(r.lightScatteringRT.get(), 3);// God rays texture
        encoder->setFragmentBytes(&gpuParams, sizeof(GPUPostProcessParams), 0);
        encoder->drawPrimitives(MTL::PrimitiveType::PrimitiveTypeTriangle, 0, 3, 1);
        encoder->endEncoding();
    }
};

// Debug draw pass: Renders wireframe debug shapes (lines)
class DebugDrawPass : public RenderPass {
public:
    explicit DebugDrawPass(Renderer_Metal* renderer) : RenderPass(renderer) {
    }

    const char* getName() const override {
        return "DebugDrawPass";
    }

    void execute() override {
        auto& r = *renderer;

        // Skip if no debug draw data
        if (!r.debugDraw || !r.debugDraw->hasContent()) {
            return;
        }

        const auto& lineVertices = r.debugDraw->getLineVertices();
        if (lineVertices.empty()) {
            return;
        }

        // Get or create vertex buffer for this frame
        auto& vertexBuffer = r.debugDrawVertexBuffers[r.currentFrameInFlight];

        // Calculate required buffer size
        size_t requiredSize = lineVertices.size() * sizeof(Vapor::DebugVertex);

        // Reallocate buffer if needed
        if (!vertexBuffer || vertexBuffer->length() < requiredSize) {
            // Allocate with some extra space to avoid frequent reallocations
            size_t allocSize = std::max(requiredSize, size_t(64 * 1024));// Min 64KB
            vertexBuffer = NS::TransferPtr(r.device->newBuffer(allocSize, MTL::ResourceStorageModeShared));
        }

        // Upload vertex data
        memcpy(vertexBuffer->contents(), lineVertices.data(), requiredSize);
        vertexBuffer->didModifyRange(NS::Range(0, requiredSize));

        // Create render pass descriptor
        auto passDesc = NS::TransferPtr(MTL::RenderPassDescriptor::renderPassDescriptor());
        auto colorAttachment = passDesc->colorAttachments()->object(0);
        colorAttachment->setTexture(r.currentDrawable->texture());
        colorAttachment->setLoadAction(MTL::LoadActionLoad);
        colorAttachment->setStoreAction(MTL::StoreActionStore);

        // Use depth buffer for proper occlusion
        auto depthAttachment = passDesc->depthAttachment();
        depthAttachment->setTexture(r.depthStencilRT.get());
        depthAttachment->setLoadAction(MTL::LoadActionLoad);
        depthAttachment->setStoreAction(MTL::StoreActionStore);

        auto encoder = r.currentCommandBuffer->renderCommandEncoder(passDesc.get());

        // Set viewport
        auto drawableSize = r.currentDrawable->texture()->width();
        auto drawableHeight = r.currentDrawable->texture()->height();
        MTL::Viewport viewport = { 0.0, 0.0, static_cast<double>(drawableSize), static_cast<double>(drawableHeight),
                                   0.0, 1.0 };
        encoder->setViewport(viewport);

        // Set pipeline and depth state
        encoder->setRenderPipelineState(r.debugDrawPipeline.get());
        encoder->setDepthStencilState(r.debugDrawDepthStencilState.get());
        encoder->setCullMode(MTL::CullModeNone);

        // Set vertex buffer
        encoder->setVertexBuffer(vertexBuffer.get(), 0, 0);
        encoder->setVertexBuffer(r.cameraDataBuffers[r.currentFrameInFlight].get(), 0, 1);

        // Draw lines
        encoder->drawPrimitives(MTL::PrimitiveTypeLine, NS::UInteger(0), NS::UInteger(lineVertices.size()));

        encoder->endEncoding();

        r.debugDraw->clear();
    }
};

// RmlUI pass: Renders the RmlUI overlay (before ImGui)
class RmlUiPass : public RenderPass {
public:
    explicit RmlUiPass(Renderer_Metal* renderer) : RenderPass(renderer) {
    }

    const char* getName() const override {
        return "RmlUiPass";
    }

    void execute() override {
        auto& r = *renderer;
        // Simply call the renderer's UI rendering method
        r.renderUI();
    }
};

// ImGui pass: Renders the ImGui UI overlay
class ImGuiPass : public RenderPass {
public:
    explicit ImGuiPass(Renderer_Metal* renderer) : RenderPass(renderer) {
    }

    const char* getName() const override {
        return "ImGuiPass";
    }

    void execute() override {
        auto& r = *renderer;

        // UI building is done in draw() before this pass
        // This pass just renders the ImGui draw data
        ImGui::Render();

        // Create render pass descriptor
        auto imguiPassDesc = NS::TransferPtr(MTL::RenderPassDescriptor::renderPassDescriptor());
        auto imguiPassColorRT = imguiPassDesc->colorAttachments()->object(0);
        imguiPassColorRT->setLoadAction(MTL::LoadActionLoad);
        imguiPassColorRT->setStoreAction(MTL::StoreActionStore);
        imguiPassColorRT->setTexture(r.currentDrawable->texture());

        auto encoder = r.currentCommandBuffer->renderCommandEncoder(imguiPassDesc.get());
        ImGui_ImplMetal_RenderDrawData(ImGui::GetDrawData(), r.currentCommandBuffer, encoder);
        encoder->endEncoding();
    }
};

// 2D Batch pass: Renders batched 2D primitives (quads, lines, shapes)
class WorldCanvasPass : public RenderPass {
public:
    explicit WorldCanvasPass(Renderer_Metal* renderer) : RenderPass(renderer) {
    }

    const char* getName() const override {
        return "WorldCanvasPass";
    }

    void execute() override {
        auto& r = *renderer;

        // Skip if no batch data
        if (r.batch3DVertices.empty() || r.batch3DIndices.empty()) {
            return;
        }

        // Use 3D buffers
        auto& vertexBuffer = r.batch3DVertexBuffers[r.currentFrameInFlight];
        auto& indexBuffer = r.batch3DIndexBuffers[r.currentFrameInFlight];
        auto& uniformBuffer = r.batch3DUniformBuffers[r.currentFrameInFlight];

        Uint32 vertexCount = static_cast<Uint32>(r.batch3DVertices.size());
        Uint32 indexCount = static_cast<Uint32>(r.batch3DIndices.size());

        size_t vertexDataSize = vertexCount * sizeof(Batch2DVertex);
        size_t indexDataSize = indexCount * sizeof(Uint32);

        if (!vertexBuffer || vertexBuffer->length() < vertexDataSize) {
            size_t allocSize = std::max(vertexDataSize, size_t(256 * 1024));
            vertexBuffer = NS::TransferPtr(r.device->newBuffer(allocSize, MTL::ResourceStorageModeShared));
        }
        if (!indexBuffer || indexBuffer->length() < indexDataSize) {
            size_t allocSize = std::max(indexDataSize, size_t(128 * 1024));
            indexBuffer = NS::TransferPtr(r.device->newBuffer(allocSize, MTL::ResourceStorageModeShared));
        }

        memcpy(vertexBuffer->contents(), r.batch3DVertices.data(), vertexDataSize);
        memcpy(indexBuffer->contents(), r.batch3DIndices.data(), indexDataSize);

        // Use camera's viewProj for 3D batch
        Batch2DUniforms uniforms;
        uniforms.projectionMatrix = r.currentCamera->getProjMatrix() * r.currentCamera->getViewMatrix();
        memcpy(uniformBuffer->contents(), &uniforms, sizeof(Batch2DUniforms));

        MTL::RenderPipelineState* pipeline = r.batch2DPipeline.get();
        if (!pipeline) return;

        auto passDesc = NS::TransferPtr(MTL::RenderPassDescriptor::renderPassDescriptor());
        auto colorAttachment = passDesc->colorAttachments()->object(0);
        colorAttachment->setTexture(r.colorRT.get());// Render to HDR RT (before bloom)
        colorAttachment->setLoadAction(MTL::LoadActionLoad);
        colorAttachment->setStoreAction(MTL::StoreActionStore);

        // Always use depth buffer for 3D
        if (r.depthStencilRT) {
            auto depthAttachment = passDesc->depthAttachment();
            depthAttachment->setTexture(r.depthStencilRT.get());
            depthAttachment->setLoadAction(MTL::LoadActionLoad);
            depthAttachment->setStoreAction(MTL::StoreActionStore);
        }

        auto encoder = r.currentCommandBuffer->renderCommandEncoder(passDesc.get());

        auto drawableWidth = r.colorRT->width();
        auto drawableHeight = r.colorRT->height();
        MTL::Viewport viewport = { 0.0, 0.0, static_cast<double>(drawableWidth), static_cast<double>(drawableHeight),
                                   0.0, 1.0 };
        encoder->setViewport(viewport);

        encoder->setRenderPipelineState(pipeline);
        encoder->setDepthStencilState(r.batch2DDepthStencilStateEnabled.get());
        encoder->setCullMode(MTL::CullModeNone);

        encoder->setVertexBuffer(vertexBuffer.get(), 0, 0);
        encoder->setVertexBuffer(uniformBuffer.get(), 0, 1);

        for (Uint32 i = 0; i < r.batch3DTextureSlotIndex; i++) {
            TextureHandle handle = r.batch3DTextureSlots[i];
            MTL::Texture* texture = r.batch2DWhiteTexture.get();
            if (handle.rid != UINT32_MAX) {
                auto texPtr = r.getTexture(handle);
                if (texPtr) texture = texPtr.get();
            }
            encoder->setFragmentTexture(texture, i);
        }

        encoder->drawIndexedPrimitives(
            MTL::PrimitiveTypeTriangle,
            NS::UInteger(indexCount),
            MTL::IndexTypeUInt32,
            indexBuffer.get(),
            NS::UInteger(0)
        );
        encoder->endEncoding();

        // Clear batch
        r.batch3DVertices.clear();
        r.batch3DIndices.clear();
        r.batch3DTextureSlotIndex = 1;
        r.batch3DActive = false;
    }
};

class CanvasPass : public RenderPass {
public:
    explicit CanvasPass(Renderer_Metal* renderer) : RenderPass(renderer) {
    }

    const char* getName() const override {
        return "CanvasPass";
    }

    void execute() override {
        auto& r = *renderer;

        // Skip if no batch data
        if (r.batch2DVertices.empty() || r.batch2DIndices.empty()) {
            return;
        }

        // Get current frame buffers
        auto& vertexBuffer = r.batch2DVertexBuffers[r.currentFrameInFlight];
        auto& indexBuffer = r.batch2DIndexBuffers[r.currentFrameInFlight];
        auto& uniformBuffer = r.batch2DUniformBuffers[r.currentFrameInFlight];

        Uint32 vertexCount = static_cast<Uint32>(r.batch2DVertices.size());
        Uint32 indexCount = static_cast<Uint32>(r.batch2DIndices.size());

        size_t vertexDataSize = vertexCount * sizeof(Batch2DVertex);
        size_t indexDataSize = indexCount * sizeof(Uint32);

        if (!vertexBuffer || vertexBuffer->length() < vertexDataSize) {
            size_t allocSize = std::max(vertexDataSize, size_t(256 * 1024));
            vertexBuffer = NS::TransferPtr(r.device->newBuffer(allocSize, MTL::ResourceStorageModeShared));
        }
        if (!indexBuffer || indexBuffer->length() < indexDataSize) {
            size_t allocSize = std::max(indexDataSize, size_t(128 * 1024));
            indexBuffer = NS::TransferPtr(r.device->newBuffer(allocSize, MTL::ResourceStorageModeShared));
        }

        memcpy(vertexBuffer->contents(), r.batch2DVertices.data(), vertexDataSize);
        memcpy(indexBuffer->contents(), r.batch2DIndices.data(), indexDataSize);

        auto rtWidth = r.colorRT->width();
        auto rtHeight = r.colorRT->height();

        // Get window size for screen space coordinates (not framebuffer size!)
        int windowWidth, windowHeight;
        SDL_GetWindowSize(r.window, &windowWidth, &windowHeight);

        // Compute projection matrix based on camera mode
        Batch2DUniforms uniforms;
        if (r.currentCamera && r.currentCamera->isOrthographic()) {
            // World space ortho: use camera's projection and view matrices
            uniforms.projectionMatrix = r.currentCamera->getProjMatrix() * r.currentCamera->getViewMatrix();
        } else {
            // Fallback: screen space ortho using window size (origin top-left, pixel coordinates)
            uniforms.projectionMatrix = glm::ortho(
                0.0f, static_cast<float>(windowWidth),
                static_cast<float>(windowHeight), 0.0f,
                -1.0f, 1.0f
            );
        }
        memcpy(uniformBuffer->contents(), &uniforms, sizeof(Batch2DUniforms));

        // Select pipeline based on blend mode
        MTL::RenderPipelineState* pipeline = r.batch2DPipeline.get();

        if (!pipeline) {
            return;
        }

        // Create render pass descriptor - render to HDR RT (before bloom)
        auto passDesc = NS::TransferPtr(MTL::RenderPassDescriptor::renderPassDescriptor());
        auto colorAttachment = passDesc->colorAttachments()->object(0);
        colorAttachment->setTexture(r.colorRT.get());// Render to HDR RT
        colorAttachment->setLoadAction(MTL::LoadActionLoad);
        colorAttachment->setStoreAction(MTL::StoreActionStore);

        auto encoder = r.currentCommandBuffer->renderCommandEncoder(passDesc.get());

        MTL::Viewport viewport = { 0.0, 0.0, static_cast<double>(rtWidth), static_cast<double>(rtHeight), 0.0, 1.0 };
        encoder->setViewport(viewport);

        encoder->setRenderPipelineState(pipeline);
        encoder->setDepthStencilState(r.batch2DDepthStencilState.get());
        encoder->setCullMode(MTL::CullModeNone);

        // Set vertex buffers
        encoder->setVertexBuffer(vertexBuffer.get(), 0, 0);
        encoder->setVertexBuffer(uniformBuffer.get(), 0, 1);

        // Bind textures
        for (Uint32 i = 0; i < r.batch2DTextureSlotIndex; i++) {
            TextureHandle handle = r.batch2DTextureSlots[i];
            MTL::Texture* texture = nullptr;
            if (handle.rid != UINT32_MAX) {
                auto texPtr = r.getTexture(handle);
                if (texPtr) {
                    texture = texPtr.get();
                }
            }
            if (!texture) {
                texture = r.batch2DWhiteTexture.get();
            }
            encoder->setFragmentTexture(texture, i);
        }

        // Draw indexed triangles
        encoder->drawIndexedPrimitives(
            MTL::PrimitiveTypeTriangle,
            NS::UInteger(indexCount),
            MTL::IndexTypeUInt32,
            indexBuffer.get(),
            NS::UInteger(0)
        );

        encoder->endEncoding();

        // Update stats
        r.batch2DStats.drawCalls++;
        r.batch2DStats.vertexCount += vertexCount;
        r.batch2DStats.indexCount += indexCount;

        // Clear batch for next frame
        r.batch2DVertices.clear();
        r.batch2DIndices.clear();
        r.batch2DTextureSlotIndex = 1;
        r.batch2DActive = false;
    }
};

std::unique_ptr<Renderer> createRendererMetal() {
    return std::make_unique<Renderer_Metal>();
}

Renderer_Metal::Renderer_Metal() {
}

Renderer_Metal::~Renderer_Metal() {
    deinit();
}

auto Renderer_Metal::init(SDL_Window* window) -> void {
    ZoneScoped;

    this->window = window;
    renderer = SDL_CreateRenderer(window, nullptr);
    swapchain = (CA::MetalLayer*)SDL_GetRenderMetalLayer(renderer);
    // swapchain->setDisplaySyncEnabled(true);
    swapchain->setPixelFormat(MTL::PixelFormatRGBA8Unorm_sRGB);
    swapchain->setColorspace(CGColorSpaceCreateWithName(kCGColorSpaceSRGB));
    device = swapchain->device();
    queue = NS::TransferPtr(device->newCommandQueue());

    // ImGui init
    ImGui_ImplSDL3_InitForMetal(window);
    ImGui_ImplMetal_Init(device);

    isInitialized = true;

    createResources();

    // Initialize render graph with all passes
    // IBL passes (run conditionally when iblNeedsUpdate is true)
    graph.addPass(std::make_unique<SkyCapturePass>(this));
    graph.addPass(std::make_unique<IrradianceConvolutionPass>(this));
    graph.addPass(std::make_unique<PrefilterEnvMapPass>(this));
    graph.addPass(std::make_unique<BRDFLUTPass>(this));

    // Scene rendering passes
    graph.addPass(std::make_unique<TLASBuildPass>(this));
    graph.addPass(std::make_unique<PrePass>(this));
    graph.addPass(std::make_unique<NormalResolvePass>(this));
    graph.addPass(std::make_unique<TileCullingPass>(this));
    graph.addPass(std::make_unique<RaytraceShadowPass>(this));
    graph.addPass(std::make_unique<RaytraceAOPass>(this));

    // GIBS (Global Illumination Based on Surfels) passes
    // These run after depth/normal are available but before main render
    if (gibsEnabled && gibsManager) {
        graph.addPass(std::make_unique<Vapor::SurfelGenerationPass>(this, gibsManager.get()));
        graph.addPass(std::make_unique<Vapor::SurfelHashBuildPass>(this, gibsManager.get()));
        graph.addPass(std::make_unique<Vapor::SurfelRaytracingPass>(this, gibsManager.get()));
        graph.addPass(std::make_unique<Vapor::GIBSTemporalPass>(this, gibsManager.get()));
        graph.addPass(std::make_unique<Vapor::GIBSSamplePass>(this, gibsManager.get()));
    }

    graph.addPass(std::make_unique<MainRenderPass>(this));
    graph.addPass(std::make_unique<SkyAtmospherePass>(this));
    // graph.addPass(std::make_unique<WaterPass>(this));
    graph.addPass(std::make_unique<ParticlePass>(this));

    // Volumetric effects (fog and clouds)
    graph.addPass(std::make_unique<VolumetricFogPass>(this));
    graph.addPass(std::make_unique<VolumetricCloudPass>(this));

    // Light scattering (god rays)
    graph.addPass(std::make_unique<LightScatteringPass>(this));
    graph.addPass(std::make_unique<WorldCanvasPass>(this));// 3D world-space quads (with depth)
    graph.addPass(std::make_unique<CanvasPass>(this));// 2D screen-space quads (no depth, for pure 2D games)

    // Bloom passes (physically-based bloom)
    graph.addPass(std::make_unique<BloomBrightnessPass>(this));
    graph.addPass(std::make_unique<BloomDownsamplePass>(this));
    graph.addPass(std::make_unique<BloomUpsamplePass>(this));
    graph.addPass(std::make_unique<BloomCompositePass>(this));

    // Sun flare / lens flare effect (after bloom)
    graph.addPass(std::make_unique<SunFlarePass>(this));

    // DOF passes (Octopath Traveler style tilt-shift)
    // Uncomment these to enable DOF, and change PostProcessPass input to dofResultRT
    // graph.addPass(std::make_unique<DOFCoCPass>(this));
    // graph.addPass(std::make_unique<DOFBlurPass>(this));
    // graph.addPass(std::make_unique<DOFCompositePass>(this));

    // Post-processing (tone mapping, color grading, chromatic aberration, vignette)
    graph.addPass(std::make_unique<PostProcessPass>(this));
    graph.addPass(std::make_unique<DebugDrawPass>(this));// Debug draw after post-process
    graph.addPass(std::make_unique<RmlUiPass>(this));// RmlUI (pure UI, no bloom)
    graph.addPass(std::make_unique<ImGuiPass>(this));

    debugDraw = std::make_shared<Vapor::DebugDraw>();

    // Initialize 2D batch state
    batch2DVertices.reserve(BatchMaxVertices);
    batch2DIndices.reserve(BatchMaxIndices);
    batch2DTextureSlots[0] = batch2DWhiteTextureHandle;
    batch2DTextureSlotIndex = 1;

    // Pre-compute quad vertex positions (centered at origin, size 1x1)
    batchQuadPositions[0] = { -0.5f, -0.5f, 0.0f, 1.0f };
    batchQuadPositions[1] = { 0.5f, -0.5f, 0.0f, 1.0f };
    batchQuadPositions[2] = { 0.5f, 0.5f, 0.0f, 1.0f };
    batchQuadPositions[3] = { -0.5f, 0.5f, 0.0f, 1.0f };

    // Default UVs
    batchQuadTexCoords[0] = { 0.0f, 0.0f };
    batchQuadTexCoords[1] = { 1.0f, 0.0f };
    batchQuadTexCoords[2] = { 1.0f, 1.0f };
    batchQuadTexCoords[3] = { 0.0f, 1.0f };
}

auto Renderer_Metal::deinit() -> void {
    if (!isInitialized) {
        return;
    }

    // UI cleanup
    if (m_uiRenderer) {
        auto* uiRenderer = static_cast<Vapor::RmlUiRenderer_Metal*>(m_uiRenderer);
        uiRenderer->Shutdown();
        delete uiRenderer;
        m_uiRenderer = nullptr;
    }

    // ImGui deinit
    ImGui_ImplMetal_Shutdown();
    ImGui_ImplSDL3_Shutdown();

    SDL_DestroyRenderer(renderer);

    isInitialized = false;
}

bool Renderer_Metal::initUI() {
    // Get the engine core and RmlUI manager
    auto* engineCore = Vapor::EngineCore::Get();
    if (!engineCore) {
        fmt::print("Renderer_Metal::initUI: EngineCore not available\n");
        return false;
    }

    auto* rmluiManager = engineCore->getRmlUiManager();
    if (!rmluiManager || !rmluiManager->IsInitialized()) {
        fmt::print("Renderer_Metal::initUI: RmlUiManager not initialized\n");
        return false;
    }

    // Create Metal UI renderer
    auto* uiRenderer = new Vapor::RmlUiRenderer_Metal(device);
    if (!uiRenderer->Initialize()) {
        fmt::print("Renderer_Metal::initUI: Failed to initialize Metal UI renderer\n");
        delete uiRenderer;
        return false;
    }

    m_uiRenderer = uiRenderer;

    // Set as RmlUI's render interface
    Rml::SetRenderInterface(uiRenderer);

    // Now finalize RmlUI initialization (creates context, loads fonts, etc.)
    if (!rmluiManager->FinalizeInitialization()) {
        fmt::print("Renderer_Metal::initUI: Failed to finalize RmlUI\n");
        delete uiRenderer;
        m_uiRenderer = nullptr;
        return false;
    }

    // Store the context
    m_uiContext = rmluiManager->GetContext();

    fmt::print("Renderer_Metal::initUI: UI renderer initialized successfully\n");
    return true;
}

void Renderer_Metal::renderUI() {
    if (!m_uiRenderer || !m_uiContext) {
        return;
    }

    auto* uiRenderer = static_cast<Vapor::RmlUiRenderer_Metal*>(m_uiRenderer);

    auto surface = currentDrawable;
    if (!surface) return;

    // Use window size for RmlUI coordinate system (not framebuffer size)
    int windowWidth, windowHeight;
    SDL_GetWindowSize(window, &windowWidth, &windowHeight);

    uiRenderer->BeginFrame(currentCommandBuffer, surface->texture(), windowWidth, windowHeight);
    m_uiContext->Render();
    uiRenderer->EndFrame();
}

auto Renderer_Metal::createResources() -> void {
    // Create pipelines
    drawPipeline = createPipeline("assets/shaders/3d_pbr_normal_mapped.metal", true, false, MSAA_SAMPLE_COUNT);
    prePassPipeline = createPipeline("assets/shaders/3d_depth_only.metal", true, false, MSAA_SAMPLE_COUNT);
    postProcessPipeline = createPipeline("assets/shaders/3d_post_process.metal", false, true, 1);
    buildClustersPipeline = createComputePipeline("assets/shaders/3d_cluster_build.metal");
    cullLightsPipeline = createComputePipeline("assets/shaders/3d_light_cull.metal");
    tileCullingPipeline = createComputePipeline("assets/shaders/3d_tile_light_cull.metal");
    normalResolvePipeline = createComputePipeline("assets/shaders/3d_normal_resolve.metal");
    raytraceShadowPipeline = createComputePipeline("assets/shaders/3d_raytrace_shadow.metal");
    raytraceAOPipeline = createComputePipeline("assets/shaders/3d_ssao.metal");
    atmospherePipeline =
        createPipeline("assets/shaders/3d_atmosphere.metal", true, false, 1);// No MSAA for sky (full-screen triangle)
    skyCapturePipeline = createPipeline("assets/shaders/3d_sky_capture.metal", true, true, 1);
    irradianceConvolutionPipeline = createPipeline("assets/shaders/3d_irradiance_convolution.metal", true, true, 1);
    prefilterEnvMapPipeline = createPipeline("assets/shaders/3d_prefilter_envmap.metal", true, true, 1);
    brdfLUTPipeline = createPipeline("assets/shaders/3d_brdf_lut.metal", false, true, 1);
    lightScatteringPipeline = createPipeline("assets/shaders/3d_light_scattering.metal", true, true, 1);

    // GIBS (Global Illumination Based on Surfels) pipelines
    surfelGenerationPipeline = createComputePipeline("assets/shaders/gibs_surfel_generation.metal");
    surfelClearCellsPipeline = createComputePipeline("assets/shaders/gibs_spatial_hash.metal");
    surfelCountPerCellPipeline = createComputePipeline("assets/shaders/gibs_spatial_hash.metal");
    surfelPrefixSumPipeline = createComputePipeline("assets/shaders/gibs_spatial_hash.metal");
    surfelScatterPipeline = createComputePipeline("assets/shaders/gibs_spatial_hash.metal");
    surfelRaytracingPipeline = createComputePipeline("assets/shaders/gibs_raytracing.metal");
    surfelRaytracingSimplePipeline = createComputePipeline("assets/shaders/gibs_raytracing.metal");
    gibsTemporalPipeline = createComputePipeline("assets/shaders/gibs_temporal.metal");
    gibsSamplePipeline = createComputePipeline("assets/shaders/gibs_sample.metal");
    gibsUpsamplePipeline = createComputePipeline("assets/shaders/gibs_sample.metal");
    gibsCompositePipeline = createComputePipeline("assets/shaders/gibs_sample.metal");

    // Initialize GIBS Manager
    gibsManager = std::make_unique<Vapor::GIBSManager>(this);
    gibsManager->setQuality(gibsQuality);
    gibsManager->init();

    // Create debug draw pipeline
    {
        auto shaderSrc = readFile("assets/shaders/3d_debug.metal");
        auto code = NS::String::string(shaderSrc.data(), NS::StringEncoding::UTF8StringEncoding);
        NS::Error* error = nullptr;
        MTL::Library* library = device->newLibrary(code, nullptr, &error);
        if (!library) {
            fmt::print(
                "Warning: Could not compile debug draw shader: {}\n",
                error ? error->localizedDescription()->utf8String() : "unknown error"
            );
        } else {
            auto vertexFuncName = NS::String::string("debug_vertex", NS::StringEncoding::UTF8StringEncoding);
            auto vertexMain = library->newFunction(vertexFuncName);

            auto fragmentFuncName = NS::String::string("debug_fragment", NS::StringEncoding::UTF8StringEncoding);
            auto fragmentMain = library->newFunction(fragmentFuncName);

            MTL::RenderPipelineDescriptor* pipelineDesc = MTL::RenderPipelineDescriptor::alloc()->init();
            pipelineDesc->setVertexFunction(vertexMain);
            pipelineDesc->setFragmentFunction(fragmentMain);
            pipelineDesc->colorAttachments()->object(0)->setPixelFormat(swapchain->pixelFormat());
            pipelineDesc->setDepthAttachmentPixelFormat(MTL::PixelFormatDepth32Float);

            // Enable blending for semi-transparent debug shapes
            auto colorAttachment = pipelineDesc->colorAttachments()->object(0);
            colorAttachment->setBlendingEnabled(true);
            colorAttachment->setRgbBlendOperation(MTL::BlendOperationAdd);
            colorAttachment->setAlphaBlendOperation(MTL::BlendOperationAdd);
            colorAttachment->setSourceRGBBlendFactor(MTL::BlendFactorSourceAlpha);
            colorAttachment->setDestinationRGBBlendFactor(MTL::BlendFactorOneMinusSourceAlpha);
            colorAttachment->setSourceAlphaBlendFactor(MTL::BlendFactorOne);
            colorAttachment->setDestinationAlphaBlendFactor(MTL::BlendFactorZero);

            debugDrawPipeline = NS::TransferPtr(device->newRenderPipelineState(pipelineDesc, &error));
            if (!debugDrawPipeline) {
                fmt::print(
                    "Warning: Could not create debug draw pipeline: {}\n",
                    error ? error->localizedDescription()->utf8String() : "unknown error"
                );
            }

            pipelineDesc->release();
            vertexMain->release();
            fragmentMain->release();
            library->release();
        }

        // Create depth stencil state for debug draw (read depth, don't write)
        MTL::DepthStencilDescriptor* depthDesc = MTL::DepthStencilDescriptor::alloc()->init();
        depthDesc->setDepthCompareFunction(MTL::CompareFunctionLessEqual);
        depthDesc->setDepthWriteEnabled(false);// Don't write to depth buffer
        debugDrawDepthStencilState = NS::TransferPtr(device->newDepthStencilState(depthDesc));
        depthDesc->release();

        // Create per-frame vertex buffers for debug draw
        debugDrawVertexBuffers.resize(MAX_FRAMES_IN_FLIGHT);
        for (auto& buffer : debugDrawVertexBuffers) {
            buffer = nullptr;// Will be allocated on demand
        }
    }

    // Create 2D batch rendering pipeline
    {
        auto shaderSrc = readFile("assets/shaders/2d_batch.metal");
        auto code = NS::String::string(shaderSrc.data(), NS::StringEncoding::UTF8StringEncoding);
        NS::Error* error = nullptr;
        MTL::Library* library = device->newLibrary(code, nullptr, &error);
        if (!library) {
            fmt::print(
                "Warning: Could not compile 2D batch shader: {}\n",
                error ? error->localizedDescription()->utf8String() : "unknown error"
            );
        } else {
            auto vertexFuncName = NS::String::string("batch2d_vertex", NS::StringEncoding::UTF8StringEncoding);
            auto vertexMain = library->newFunction(vertexFuncName);

            auto fragmentFuncName = NS::String::string("batch2d_fragment", NS::StringEncoding::UTF8StringEncoding);
            auto fragmentMain = library->newFunction(fragmentFuncName);

            if (!vertexMain || !fragmentMain) {
                fmt::print("Warning: Could not find batch2d shader functions\n");
            } else {
                // Create pipeline with alpha blending (default)
                {
                    MTL::RenderPipelineDescriptor* pipelineDesc = MTL::RenderPipelineDescriptor::alloc()->init();
                    pipelineDesc->setVertexFunction(vertexMain);
                    pipelineDesc->setFragmentFunction(fragmentMain);
                    pipelineDesc->colorAttachments()->object(0)->setPixelFormat(MTL::PixelFormatRGBA16Float);

                    auto colorAttachment = pipelineDesc->colorAttachments()->object(0);
                    colorAttachment->setBlendingEnabled(true);
                    colorAttachment->setRgbBlendOperation(MTL::BlendOperationAdd);
                    colorAttachment->setAlphaBlendOperation(MTL::BlendOperationAdd);
                    colorAttachment->setSourceRGBBlendFactor(MTL::BlendFactorSourceAlpha);
                    colorAttachment->setDestinationRGBBlendFactor(MTL::BlendFactorOneMinusSourceAlpha);
                    colorAttachment->setSourceAlphaBlendFactor(MTL::BlendFactorOne);
                    colorAttachment->setDestinationAlphaBlendFactor(MTL::BlendFactorOneMinusSourceAlpha);

                    batch2DPipeline = NS::TransferPtr(device->newRenderPipelineState(pipelineDesc, &error));
                    if (!batch2DPipeline) {
                        fmt::print(
                            "Warning: Could not create 2D batch pipeline: {}\n",
                            error ? error->localizedDescription()->utf8String() : "unknown error"
                        );
                    }
                    pipelineDesc->release();
                }

                // Create pipeline with additive blending
                {
                    MTL::RenderPipelineDescriptor* pipelineDesc = MTL::RenderPipelineDescriptor::alloc()->init();
                    pipelineDesc->setVertexFunction(vertexMain);
                    pipelineDesc->setFragmentFunction(fragmentMain);
                    pipelineDesc->colorAttachments()->object(0)->setPixelFormat(MTL::PixelFormatRGBA16Float);

                    auto colorAttachment = pipelineDesc->colorAttachments()->object(0);
                    colorAttachment->setBlendingEnabled(true);
                    colorAttachment->setRgbBlendOperation(MTL::BlendOperationAdd);
                    colorAttachment->setAlphaBlendOperation(MTL::BlendOperationAdd);
                    colorAttachment->setSourceRGBBlendFactor(MTL::BlendFactorSourceAlpha);
                    colorAttachment->setDestinationRGBBlendFactor(MTL::BlendFactorOne);
                    colorAttachment->setSourceAlphaBlendFactor(MTL::BlendFactorOne);
                    colorAttachment->setDestinationAlphaBlendFactor(MTL::BlendFactorOne);

                    batch2DPipelineAdditive = NS::TransferPtr(device->newRenderPipelineState(pipelineDesc, &error));
                    pipelineDesc->release();
                }

                // Create pipeline with multiply blending
                {
                    MTL::RenderPipelineDescriptor* pipelineDesc = MTL::RenderPipelineDescriptor::alloc()->init();
                    pipelineDesc->setVertexFunction(vertexMain);
                    pipelineDesc->setFragmentFunction(fragmentMain);
                    pipelineDesc->colorAttachments()->object(0)->setPixelFormat(MTL::PixelFormatRGBA16Float);

                    auto colorAttachment = pipelineDesc->colorAttachments()->object(0);
                    colorAttachment->setBlendingEnabled(true);
                    colorAttachment->setRgbBlendOperation(MTL::BlendOperationAdd);
                    colorAttachment->setAlphaBlendOperation(MTL::BlendOperationAdd);
                    colorAttachment->setSourceRGBBlendFactor(MTL::BlendFactorDestinationColor);
                    colorAttachment->setDestinationRGBBlendFactor(MTL::BlendFactorZero);
                    colorAttachment->setSourceAlphaBlendFactor(MTL::BlendFactorOne);
                    colorAttachment->setDestinationAlphaBlendFactor(MTL::BlendFactorZero);

                    batch2DPipelineMultiply = NS::TransferPtr(device->newRenderPipelineState(pipelineDesc, &error));
                    pipelineDesc->release();
                }

                vertexMain->release();
                fragmentMain->release();
            }
            library->release();
        }

        // Create depth stencil state for 2D batch (no depth testing/writing)
        MTL::DepthStencilDescriptor* depthDesc = MTL::DepthStencilDescriptor::alloc()->init();
        depthDesc->setDepthCompareFunction(MTL::CompareFunctionAlways);
        depthDesc->setDepthWriteEnabled(false);
        batch2DDepthStencilState = NS::TransferPtr(device->newDepthStencilState(depthDesc));
        depthDesc->release();

        // Create depth stencil state for 2D batch with depth testing (for world UI)
        MTL::DepthStencilDescriptor* depthDescEnabled = MTL::DepthStencilDescriptor::alloc()->init();
        depthDescEnabled->setDepthCompareFunction(MTL::CompareFunctionLessEqual);
        depthDescEnabled->setDepthWriteEnabled(true);
        batch2DDepthStencilStateEnabled = NS::TransferPtr(device->newDepthStencilState(depthDescEnabled));
        depthDescEnabled->release();

        // Create per-frame buffers for 2D batch
        batch2DVertexBuffers.resize(MAX_FRAMES_IN_FLIGHT);
        batch2DIndexBuffers.resize(MAX_FRAMES_IN_FLIGHT);
        batch2DUniformBuffers.resize(MAX_FRAMES_IN_FLIGHT);
        batch3DVertexBuffers.resize(MAX_FRAMES_IN_FLIGHT);
        batch3DIndexBuffers.resize(MAX_FRAMES_IN_FLIGHT);
        batch3DUniformBuffers.resize(MAX_FRAMES_IN_FLIGHT);
        for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
            batch2DVertexBuffers[i] = nullptr;// Allocated on demand
            batch2DIndexBuffers[i] = nullptr;// Allocated on demand
            batch2DUniformBuffers[i] =
                NS::TransferPtr(device->newBuffer(sizeof(Batch2DUniforms), MTL::ResourceStorageModeShared));

            batch3DVertexBuffers[i] = nullptr;// Allocated on demand
            batch3DIndexBuffers[i] = nullptr;// Allocated on demand
            batch3DUniformBuffers[i] =
                NS::TransferPtr(device->newBuffer(sizeof(Batch2DUniforms), MTL::ResourceStorageModeShared));
        }

        // Create 1x1 white texture for untextured primitives
        MTL::TextureDescriptor* texDesc = MTL::TextureDescriptor::alloc()->init();
        texDesc->setWidth(1);
        texDesc->setHeight(1);
        texDesc->setPixelFormat(MTL::PixelFormatRGBA8Unorm);
        texDesc->setTextureType(MTL::TextureType2D);
        texDesc->setStorageMode(MTL::StorageModeShared);
        texDesc->setUsage(MTL::TextureUsageShaderRead);

        batch2DWhiteTexture = NS::TransferPtr(device->newTexture(texDesc));
        texDesc->release();

        // Fill with white pixel
        uint32_t whitePixel = 0xFFFFFFFF;
        batch2DWhiteTexture->replaceRegion(MTL::Region(0, 0, 1, 1), 0, &whitePixel, sizeof(uint32_t));

        // Create texture handle for the white texture
        batch2DWhiteTextureHandle.rid = nextTextureID++;
        textures[batch2DWhiteTextureHandle.rid] = batch2DWhiteTexture;

        fmt::print("2D batch rendering pipeline initialized\n");
    }

    // Create buffers
    frameDataBuffers.resize(MAX_FRAMES_IN_FLIGHT);
    for (auto& frameDataBuffer : frameDataBuffers) {
        frameDataBuffer = NS::TransferPtr(device->newBuffer(sizeof(FrameData), MTL::ResourceStorageModeManaged));
    }
    cameraDataBuffers.resize(MAX_FRAMES_IN_FLIGHT);
    for (auto& cameraDataBuffer : cameraDataBuffers) {
        cameraDataBuffer = NS::TransferPtr(device->newBuffer(sizeof(CameraData), MTL::ResourceStorageModeManaged));
    }
    instanceDataBuffers.resize(MAX_FRAMES_IN_FLIGHT);
    for (auto& instanceDataBuffer : instanceDataBuffers) {
        instanceDataBuffer =
            NS::TransferPtr(device->newBuffer(sizeof(InstanceData) * MAX_INSTANCES, MTL::ResourceStorageModeManaged));
    }

    std::vector<Particle> particles{ 1000 };
    testStorageBuffer =
        NS::TransferPtr(device->newBuffer(particles.size() * sizeof(Particle), MTL::ResourceStorageModeManaged));
    memcpy(testStorageBuffer->contents(), particles.data(), particles.size() * sizeof(Particle));
    testStorageBuffer->didModifyRange(NS::Range::Make(0, testStorageBuffer->length()));

    clusterBuffers.resize(MAX_FRAMES_IN_FLIGHT);
    for (auto& clusterBuffer : clusterBuffers) {
        clusterBuffer = NS::TransferPtr(device->newBuffer(
            clusterGridSizeX * clusterGridSizeY * clusterGridSizeZ * sizeof(Cluster), MTL::ResourceStorageModeManaged
        ));
    }

    // Create light scattering data buffers and initialize default settings
    lightScatteringDataBuffers.resize(MAX_FRAMES_IN_FLIGHT);
    for (auto& lsBuffer : lightScatteringDataBuffers) {
        lsBuffer = NS::TransferPtr(device->newBuffer(sizeof(LightScatteringData), MTL::ResourceStorageModeManaged));
    }

    // Initialize light scattering default settings
    lightScatteringSettings.sunScreenPos = glm::vec2(0.5f, 0.5f);
    lightScatteringSettings.screenSize = glm::vec2(1920.0f, 1080.0f);
    lightScatteringSettings.density = 1.0f;
    lightScatteringSettings.weight = 0.05f;
    lightScatteringSettings.decay = 0.97f;
    lightScatteringSettings.exposure = 0.3f;
    lightScatteringSettings.numSamples = 64;
    lightScatteringSettings.maxDistance = 1.0f;
    lightScatteringSettings.sunIntensity = 1.0f;
    lightScatteringSettings.mieG = 0.76f;
    lightScatteringSettings.sunColor = glm::vec3(1.0f, 0.95f, 0.9f);
    lightScatteringSettings.depthThreshold = 0.9999f;
    lightScatteringSettings.jitter = 0.5f;

    // ========================================================================
    // Volumetric Fog buffers and initialization
    // ========================================================================
    volumetricFogDataBuffers.resize(MAX_FRAMES_IN_FLIGHT);
    for (auto& fogBuffer : volumetricFogDataBuffers) {
        fogBuffer = NS::TransferPtr(device->newBuffer(sizeof(VolumetricFogData), MTL::ResourceStorageModeManaged));
    }

    // Initialize volumetric fog default settings
    volumetricFogSettings.fogDensity = 0.02f;
    volumetricFogSettings.fogHeightFalloff = 0.1f;
    volumetricFogSettings.fogBaseHeight = 0.0f;
    volumetricFogSettings.fogMaxHeight = 100.0f;
    volumetricFogSettings.scatteringCoeff = 0.5f;
    volumetricFogSettings.extinctionCoeff = 0.5f;
    volumetricFogSettings.anisotropy = 0.6f;
    volumetricFogSettings.ambientIntensity = 0.3f;
    volumetricFogSettings.nearPlane = 0.1f;
    volumetricFogSettings.farPlane = 500.0f;
    volumetricFogSettings.noiseScale = 0.01f;
    volumetricFogSettings.noiseIntensity = 0.5f;
    volumetricFogSettings.windSpeed = 1.0f;
    volumetricFogSettings.windDirection = glm::vec3(1.0f, 0.0f, 0.0f);
    volumetricFogSettings.temporalBlend = 0.1f;

    // ========================================================================
    // Volumetric Cloud buffers and initialization
    // ========================================================================
    volumetricCloudDataBuffers.resize(MAX_FRAMES_IN_FLIGHT);
    for (auto& cloudBuffer : volumetricCloudDataBuffers) {
        cloudBuffer = NS::TransferPtr(device->newBuffer(sizeof(VolumetricCloudData), MTL::ResourceStorageModeManaged));
    }

    // Initialize volumetric cloud default settings
    volumetricCloudSettings.cloudLayerBottom = 2000.0f;
    volumetricCloudSettings.cloudLayerTop = 12000.0f;
    volumetricCloudSettings.cloudLayerThickness = 2500.0f;
    volumetricCloudSettings.cloudCoverage = 0.25f;
    volumetricCloudSettings.cloudDensity = 0.3f;
    volumetricCloudSettings.cloudType = 0.5f;
    volumetricCloudSettings.erosionStrength = 0.3f;
    volumetricCloudSettings.shapeNoiseScale = 1.0f;
    volumetricCloudSettings.detailNoiseScale = 5.0f;
    volumetricCloudSettings.ambientIntensity = 0.001f;
    volumetricCloudSettings.silverLiningIntensity = 0.001f;
    volumetricCloudSettings.silverLiningSpread = 2.0f;
    volumetricCloudSettings.phaseG1 = 0.8f;
    volumetricCloudSettings.phaseG2 = -0.3f;
    volumetricCloudSettings.phaseBlend = 0.3f;
    volumetricCloudSettings.powderStrength = 0.5f;
    volumetricCloudSettings.windDirection = glm::vec3(1.0f, 0.0f, 0.0f);
    volumetricCloudSettings.windSpeed = 10.0f;
    volumetricCloudSettings.windOffset = glm::vec3(0.0f);
    volumetricCloudSettings.primarySteps = 64;
    volumetricCloudSettings.lightSteps = 6;
    volumetricCloudSettings.temporalBlend = 0.05f;

    // ========================================================================
    // Sun Flare buffers and initialization
    // ========================================================================
    sunFlareDataBuffers.resize(MAX_FRAMES_IN_FLIGHT);
    for (auto& flareBuffer : sunFlareDataBuffers) {
        flareBuffer = NS::TransferPtr(device->newBuffer(sizeof(SunFlareData), MTL::ResourceStorageModeManaged));
    }

    // Initialize sun flare default settings
    sunFlareSettings.sunIntensity = 1.0f;
    sunFlareSettings.visibility = 1.0f;
    sunFlareSettings.fadeEdge = 0.8f;
    sunFlareSettings.sunColor = glm::vec3(1.0f, 0.95f, 0.8f);
    sunFlareSettings.glowIntensity = 0.5f;
    sunFlareSettings.glowFalloff = 8.0f;
    sunFlareSettings.glowSize = 0.15f;
    sunFlareSettings.haloIntensity = 0.08f;
    sunFlareSettings.haloRadius = 0.09f;
    sunFlareSettings.haloWidth = 0.001f;
    sunFlareSettings.haloFalloff = 0.01f;
    sunFlareSettings.ghostCount = 10;
    sunFlareSettings.ghostSpacing = 0.3f;
    sunFlareSettings.ghostIntensity = 0.02f;
    sunFlareSettings.ghostSize = 0.3f;
    sunFlareSettings.ghostChromaticOffset = 0.015f;
    sunFlareSettings.ghostFalloff = 2.5f;
    sunFlareSettings.streakIntensity = 0.2f;
    sunFlareSettings.streakLength = 0.3f;
    sunFlareSettings.streakFalloff = 50.0f;
    sunFlareSettings.starburstIntensity = 0.15f;
    sunFlareSettings.starburstSize = 0.4f;
    sunFlareSettings.starburstPoints = 6;
    sunFlareSettings.starburstRotation = 0.0f;
    sunFlareSettings.dirtIntensity = 0.0f;
    sunFlareSettings.dirtScale = 10.0f;

    // Create atmosphere data buffer with default Earth-like settings
    atmosphereDataBuffer = NS::TransferPtr(device->newBuffer(sizeof(AtmosphereData), MTL::ResourceStorageModeManaged));
    AtmosphereData* atmosphereData = reinterpret_cast<AtmosphereData*>(atmosphereDataBuffer->contents());
    atmosphereData->sunDirection = glm::normalize(glm::vec3(0.5f, 0.5f, 0.5f));
    atmosphereData->sunIntensity = 12.0f;
    atmosphereData->sunColor = glm::vec3(1.0f, 1.0f, 1.0f);
    atmosphereData->planetRadius = 6371e3f;// Earth radius in meters
    atmosphereData->atmosphereRadius = 6471e3f;// Atmosphere radius (100km above surface)
    atmosphereData->rayleighScaleHeight = 8500.0f;// Rayleigh scale height
    atmosphereData->mieScaleHeight = 1200.0f;// Mie scale height
    atmosphereData->miePreferredDirection = 0.758f;// Mie phase function g parameter
    atmosphereData->rayleighCoefficients = glm::vec3(5.8e-6f, 13.5e-6f, 33.1e-6f);
    atmosphereData->mieCoefficient = 21e-6f;
    atmosphereData->exposure = 1.0f;
    atmosphereData->groundColor = glm::vec3(0.015f, 0.015f, 0.02f);// Default dark blue
    atmosphereDataBuffer->didModifyRange(NS::Range::Make(0, atmosphereDataBuffer->length()));

    // Create IBL capture data buffer
    iblCaptureDataBuffer = NS::TransferPtr(device->newBuffer(sizeof(IBLCaptureData), MTL::ResourceStorageModeManaged));

    // Create IBL textures
    const uint32_t envMapSize = 512;
    const uint32_t irradianceMapSize = 32;
    const uint32_t prefilterMapSize = 128;
    const uint32_t brdfLUTSize = 512;
    const uint32_t prefilterMipLevels = 5;

    // Environment cubemap (captured from atmosphere)
    MTL::TextureDescriptor* envMapDesc = MTL::TextureDescriptor::alloc()->init();
    envMapDesc->setTextureType(MTL::TextureTypeCube);
    envMapDesc->setPixelFormat(MTL::PixelFormatRGBA16Float);
    envMapDesc->setWidth(envMapSize);
    envMapDesc->setHeight(envMapSize);
    envMapDesc->setMipmapLevelCount(calculateMipmapLevelCount(envMapSize, envMapSize));
    envMapDesc->setUsage(MTL::TextureUsageRenderTarget | MTL::TextureUsageShaderRead);
    envMapDesc->setStorageMode(MTL::StorageModePrivate);
    environmentCubemap = NS::TransferPtr(device->newTexture(envMapDesc));
    envMapDesc->release();

    // Irradiance cubemap (diffuse IBL)
    MTL::TextureDescriptor* irradianceDesc = MTL::TextureDescriptor::alloc()->init();
    irradianceDesc->setTextureType(MTL::TextureTypeCube);
    irradianceDesc->setPixelFormat(MTL::PixelFormatRGBA16Float);
    irradianceDesc->setWidth(irradianceMapSize);
    irradianceDesc->setHeight(irradianceMapSize);
    irradianceDesc->setMipmapLevelCount(1);
    irradianceDesc->setUsage(MTL::TextureUsageRenderTarget | MTL::TextureUsageShaderRead);
    irradianceDesc->setStorageMode(MTL::StorageModePrivate);
    irradianceMap = NS::TransferPtr(device->newTexture(irradianceDesc));
    irradianceDesc->release();

    // Pre-filtered environment cubemap (specular IBL)
    MTL::TextureDescriptor* prefilterDesc = MTL::TextureDescriptor::alloc()->init();
    prefilterDesc->setTextureType(MTL::TextureTypeCube);
    prefilterDesc->setPixelFormat(MTL::PixelFormatRGBA16Float);
    prefilterDesc->setWidth(prefilterMapSize);
    prefilterDesc->setHeight(prefilterMapSize);
    prefilterDesc->setMipmapLevelCount(prefilterMipLevels);
    prefilterDesc->setUsage(MTL::TextureUsageRenderTarget | MTL::TextureUsageShaderRead);
    prefilterDesc->setStorageMode(MTL::StorageModePrivate);
    prefilterMap = NS::TransferPtr(device->newTexture(prefilterDesc));
    prefilterDesc->release();

    // BRDF LUT (2D texture)
    MTL::TextureDescriptor* brdfDesc = MTL::TextureDescriptor::alloc()->init();
    brdfDesc->setTextureType(MTL::TextureType2D);
    brdfDesc->setPixelFormat(MTL::PixelFormatRG16Float);
    brdfDesc->setWidth(brdfLUTSize);
    brdfDesc->setHeight(brdfLUTSize);
    brdfDesc->setMipmapLevelCount(1);
    brdfDesc->setUsage(MTL::TextureUsageRenderTarget | MTL::TextureUsageShaderRead);
    brdfDesc->setStorageMode(MTL::StorageModePrivate);
    brdfLUT = NS::TransferPtr(device->newTexture(brdfDesc));
    brdfDesc->release();

    accelInstanceBuffers.resize(MAX_FRAMES_IN_FLIGHT);
    TLASScratchBuffers.resize(MAX_FRAMES_IN_FLIGHT);
    TLASBuffers.resize(MAX_FRAMES_IN_FLIGHT);
    for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        accelInstanceBuffers[i] = NS::TransferPtr(device->newBuffer(
            MAX_INSTANCES * sizeof(MTL::AccelerationStructureInstanceDescriptor), MTL::ResourceStorageModeManaged
        ));
        TLASScratchBuffers[i] = nullptr;
        TLASBuffers[i] = nullptr;
    }

    // Create textures
    defaultAlbedoTexture = createTexture(AssetManager::loadImage("assets/textures/default_albedo.png")
    );// createTexture(AssetManager::loadImage("assets/textures/viking_room.png"));
    defaultNormalTexture = createTexture(AssetManager::loadImage("assets/textures/default_norm.png"));
    defaultORMTexture = createTexture(AssetManager::loadImage("assets/textures/default_orm.png"));
    defaultEmissiveTexture = createTexture(AssetManager::loadImage("assets/textures/default_emissive.png"));

    MTL::TextureDescriptor* depthStencilTextureDesc = MTL::TextureDescriptor::alloc()->init();
    depthStencilTextureDesc->setTextureType(MTL::TextureType2DMultisample);
    depthStencilTextureDesc->setPixelFormat(MTL::PixelFormatDepth32Float);
    depthStencilTextureDesc->setWidth(swapchain->drawableSize().width);
    depthStencilTextureDesc->setHeight(swapchain->drawableSize().height);
    depthStencilTextureDesc->setSampleCount(NS::UInteger(MSAA_SAMPLE_COUNT));
    depthStencilTextureDesc->setUsage(MTL::TextureUsageRenderTarget);
    depthStencilRT_MS = NS::TransferPtr(device->newTexture(depthStencilTextureDesc));
    depthStencilTextureDesc->setTextureType(MTL::TextureType2D);
    depthStencilTextureDesc->setSampleCount(1);
    depthStencilTextureDesc->setUsage(MTL::TextureUsageRenderTarget | MTL::TextureUsageShaderRead);
    depthStencilRT = NS::TransferPtr(device->newTexture(depthStencilTextureDesc));
    depthStencilTextureDesc->release();

    MTL::TextureDescriptor* colorTextureDesc = MTL::TextureDescriptor::alloc()->init();
    colorTextureDesc->setTextureType(MTL::TextureType2DMultisample);
    colorTextureDesc->setPixelFormat(MTL::PixelFormatRGBA16Float);// HDR format
    colorTextureDesc->setWidth(swapchain->drawableSize().width);
    colorTextureDesc->setHeight(swapchain->drawableSize().height);
    colorTextureDesc->setSampleCount(NS::UInteger(MSAA_SAMPLE_COUNT));
    colorTextureDesc->setUsage(MTL::TextureUsageRenderTarget);
    colorRT_MS = NS::TransferPtr(device->newTexture(colorTextureDesc));
    colorTextureDesc->setTextureType(MTL::TextureType2D);
    colorTextureDesc->setSampleCount(1);
    colorTextureDesc->setUsage(MTL::TextureUsageRenderTarget | MTL::TextureUsageShaderRead);
    colorRT = NS::TransferPtr(device->newTexture(colorTextureDesc));
    // Create tempColorRT for ping-pong post-processing (same format as colorRT)
    tempColorRT = NS::TransferPtr(device->newTexture(colorTextureDesc));
    colorTextureDesc->release();

    MTL::TextureDescriptor* normalTextureDesc = MTL::TextureDescriptor::alloc()->init();
    normalTextureDesc->setTextureType(MTL::TextureType2DMultisample);
    normalTextureDesc->setPixelFormat(MTL::PixelFormatRGBA16Float);// HDR format
    normalTextureDesc->setWidth(swapchain->drawableSize().width);
    normalTextureDesc->setHeight(swapchain->drawableSize().height);
    normalTextureDesc->setSampleCount(NS::UInteger(MSAA_SAMPLE_COUNT));
    normalTextureDesc->setUsage(MTL::TextureUsageRenderTarget | MTL::TextureUsageShaderRead);
    normalRT_MS = NS::TransferPtr(device->newTexture(normalTextureDesc));
    normalTextureDesc->setTextureType(MTL::TextureType2D);
    normalTextureDesc->setSampleCount(1);
    normalTextureDesc->setUsage(MTL::TextureUsageShaderRead | MTL::TextureUsageShaderWrite);
    normalRT = NS::TransferPtr(device->newTexture(normalTextureDesc));
    normalTextureDesc->release();

    MTL::TextureDescriptor* shadowTextureDesc = MTL::TextureDescriptor::alloc()->init();
    shadowTextureDesc->setTextureType(MTL::TextureType2D);
    shadowTextureDesc->setPixelFormat(MTL::PixelFormatRGBA8Unorm);
    shadowTextureDesc->setWidth(swapchain->drawableSize().width);
    shadowTextureDesc->setHeight(swapchain->drawableSize().height);
    shadowTextureDesc->setMipmapLevelCount(
        calculateMipmapLevelCount(swapchain->drawableSize().width, swapchain->drawableSize().height)
    );
    shadowTextureDesc->setUsage(MTL::TextureUsageShaderRead | MTL::TextureUsageShaderWrite);
    shadowRT = NS::TransferPtr(device->newTexture(shadowTextureDesc));
    shadowTextureDesc->release();

    MTL::TextureDescriptor* aoTextureDesc = MTL::TextureDescriptor::alloc()->init();
    aoTextureDesc->setTextureType(MTL::TextureType2D);
    aoTextureDesc->setPixelFormat(MTL::PixelFormatR16Float);
    aoTextureDesc->setWidth(swapchain->drawableSize().width);
    aoTextureDesc->setHeight(swapchain->drawableSize().height);
    aoTextureDesc->setUsage(MTL::TextureUsageShaderRead | MTL::TextureUsageShaderWrite);
    aoRT = NS::TransferPtr(device->newTexture(aoTextureDesc));
    aoTextureDesc->release();

    // Create light scattering render target (HDR format for god rays)
    MTL::TextureDescriptor* lightScatteringTextureDesc = MTL::TextureDescriptor::alloc()->init();
    lightScatteringTextureDesc->setTextureType(MTL::TextureType2D);
    lightScatteringTextureDesc->setPixelFormat(MTL::PixelFormatRGBA16Float);// HDR for bright rays
    lightScatteringTextureDesc->setWidth(swapchain->drawableSize().width);
    lightScatteringTextureDesc->setHeight(swapchain->drawableSize().height);
    lightScatteringTextureDesc->setUsage(MTL::TextureUsageRenderTarget | MTL::TextureUsageShaderRead);
    lightScatteringRT = NS::TransferPtr(device->newTexture(lightScatteringTextureDesc));
    lightScatteringTextureDesc->release();

    // ========================================================================
    // Bloom render targets
    // ========================================================================

    // Brightness extraction RT (half resolution)
    {
        MTL::TextureDescriptor* bloomBrightnessDesc = MTL::TextureDescriptor::alloc()->init();
        bloomBrightnessDesc->setTextureType(MTL::TextureType2D);
        bloomBrightnessDesc->setPixelFormat(MTL::PixelFormatRGBA16Float);
        bloomBrightnessDesc->setWidth(swapchain->drawableSize().width / 2);
        bloomBrightnessDesc->setHeight(swapchain->drawableSize().height / 2);
        bloomBrightnessDesc->setUsage(MTL::TextureUsageRenderTarget | MTL::TextureUsageShaderRead);
        bloomBrightnessRT = NS::TransferPtr(device->newTexture(bloomBrightnessDesc));
        bloomBrightnessDesc->release();
    }

    // Bloom pyramid render targets (progressively smaller)
    bloomPyramidRTs.resize(BLOOM_PYRAMID_LEVELS);
    for (Uint32 i = 0; i < BLOOM_PYRAMID_LEVELS; i++) {
        Uint32 width = swapchain->drawableSize().width / (1 << (i + 1));
        Uint32 height = swapchain->drawableSize().height / (1 << (i + 1));
        width = std::max(width, 1u);
        height = std::max(height, 1u);

        MTL::TextureDescriptor* pyramidDesc = MTL::TextureDescriptor::alloc()->init();
        pyramidDesc->setTextureType(MTL::TextureType2D);
        pyramidDesc->setPixelFormat(MTL::PixelFormatRGBA16Float);
        pyramidDesc->setWidth(width);
        pyramidDesc->setHeight(height);
        pyramidDesc->setUsage(MTL::TextureUsageRenderTarget | MTL::TextureUsageShaderRead);
        bloomPyramidRTs[i] = NS::TransferPtr(device->newTexture(pyramidDesc));
        pyramidDesc->release();
    }

    // Final bloom result RT (full resolution)
    {
        MTL::TextureDescriptor* bloomResultDesc = MTL::TextureDescriptor::alloc()->init();
        bloomResultDesc->setTextureType(MTL::TextureType2D);
        bloomResultDesc->setPixelFormat(MTL::PixelFormatRGBA16Float);
        bloomResultDesc->setWidth(swapchain->drawableSize().width);
        bloomResultDesc->setHeight(swapchain->drawableSize().height);
        bloomResultDesc->setUsage(MTL::TextureUsageRenderTarget | MTL::TextureUsageShaderRead);
        bloomResultRT = NS::TransferPtr(device->newTexture(bloomResultDesc));
        bloomResultDesc->release();
    }

    // ========================================================================
    // Bloom pipelines
    // ========================================================================

    // Bloom brightness pipeline
    {
        auto shaderSrc = readFile("assets/shaders/3d_bloom_brightness.metal");
        auto code = NS::String::string(shaderSrc.data(), NS::StringEncoding::UTF8StringEncoding);
        NS::Error* error = nullptr;
        MTL::Library* library = device->newLibrary(code, nullptr, &error);
        if (!library) {
            throw std::runtime_error(fmt::format(
                "Could not compile bloom brightness shader! Error: {}\n", error->localizedDescription()->utf8String()
            ));
        }

        auto vertexMain =
            library->newFunction(NS::String::string("vertexMain", NS::StringEncoding::UTF8StringEncoding));
        auto fragmentMain =
            library->newFunction(NS::String::string("fragmentMain", NS::StringEncoding::UTF8StringEncoding));

        auto pipelineDesc = MTL::RenderPipelineDescriptor::alloc()->init();
        pipelineDesc->setVertexFunction(vertexMain);
        pipelineDesc->setFragmentFunction(fragmentMain);
        pipelineDesc->colorAttachments()->object(0)->setPixelFormat(MTL::PixelFormatRGBA16Float);

        bloomBrightnessPipeline = NS::TransferPtr(device->newRenderPipelineState(pipelineDesc, &error));
        if (!bloomBrightnessPipeline) {
            throw std::runtime_error(fmt::format(
                "Could not create bloom brightness pipeline! Error: {}\n", error->localizedDescription()->utf8String()
            ));
        }

        code->release();
        library->release();
        vertexMain->release();
        fragmentMain->release();
        pipelineDesc->release();
    }

    // Bloom downsample pipeline
    {
        auto shaderSrc = readFile("assets/shaders/3d_bloom_downsample.metal");
        auto code = NS::String::string(shaderSrc.data(), NS::StringEncoding::UTF8StringEncoding);
        NS::Error* error = nullptr;
        MTL::Library* library = device->newLibrary(code, nullptr, &error);
        if (!library) {
            throw std::runtime_error(fmt::format(
                "Could not compile bloom downsample shader! Error: {}\n", error->localizedDescription()->utf8String()
            ));
        }

        auto vertexMain =
            library->newFunction(NS::String::string("vertexMain", NS::StringEncoding::UTF8StringEncoding));
        auto fragmentMain =
            library->newFunction(NS::String::string("fragmentMain", NS::StringEncoding::UTF8StringEncoding));

        auto pipelineDesc = MTL::RenderPipelineDescriptor::alloc()->init();
        pipelineDesc->setVertexFunction(vertexMain);
        pipelineDesc->setFragmentFunction(fragmentMain);
        pipelineDesc->colorAttachments()->object(0)->setPixelFormat(MTL::PixelFormatRGBA16Float);

        bloomDownsamplePipeline = NS::TransferPtr(device->newRenderPipelineState(pipelineDesc, &error));
        if (!bloomDownsamplePipeline) {
            throw std::runtime_error(fmt::format(
                "Could not create bloom downsample pipeline! Error: {}\n", error->localizedDescription()->utf8String()
            ));
        }

        code->release();
        library->release();
        vertexMain->release();
        fragmentMain->release();
        pipelineDesc->release();
    }

    // Bloom upsample pipeline
    {
        auto shaderSrc = readFile("assets/shaders/3d_bloom_upsample.metal");
        auto code = NS::String::string(shaderSrc.data(), NS::StringEncoding::UTF8StringEncoding);
        NS::Error* error = nullptr;
        MTL::Library* library = device->newLibrary(code, nullptr, &error);
        if (!library) {
            throw std::runtime_error(fmt::format(
                "Could not compile bloom upsample shader! Error: {}\n", error->localizedDescription()->utf8String()
            ));
        }

        auto vertexMain =
            library->newFunction(NS::String::string("vertexMain", NS::StringEncoding::UTF8StringEncoding));
        auto fragmentMain =
            library->newFunction(NS::String::string("fragmentMain", NS::StringEncoding::UTF8StringEncoding));

        auto pipelineDesc = MTL::RenderPipelineDescriptor::alloc()->init();
        pipelineDesc->setVertexFunction(vertexMain);
        pipelineDesc->setFragmentFunction(fragmentMain);
        pipelineDesc->colorAttachments()->object(0)->setPixelFormat(MTL::PixelFormatRGBA16Float);

        bloomUpsamplePipeline = NS::TransferPtr(device->newRenderPipelineState(pipelineDesc, &error));
        if (!bloomUpsamplePipeline) {
            throw std::runtime_error(fmt::format(
                "Could not create bloom upsample pipeline! Error: {}\n", error->localizedDescription()->utf8String()
            ));
        }

        code->release();
        library->release();
        vertexMain->release();
        fragmentMain->release();
        pipelineDesc->release();
    }

    // Bloom composite pipeline
    {
        auto shaderSrc = readFile("assets/shaders/3d_bloom_composite.metal");
        auto code = NS::String::string(shaderSrc.data(), NS::StringEncoding::UTF8StringEncoding);
        NS::Error* error = nullptr;
        MTL::Library* library = device->newLibrary(code, nullptr, &error);
        if (!library) {
            throw std::runtime_error(fmt::format(
                "Could not compile bloom composite shader! Error: {}\n", error->localizedDescription()->utf8String()
            ));
        }

        auto vertexMain =
            library->newFunction(NS::String::string("vertexMain", NS::StringEncoding::UTF8StringEncoding));
        auto fragmentMain =
            library->newFunction(NS::String::string("fragmentMain", NS::StringEncoding::UTF8StringEncoding));

        auto pipelineDesc = MTL::RenderPipelineDescriptor::alloc()->init();
        pipelineDesc->setVertexFunction(vertexMain);
        pipelineDesc->setFragmentFunction(fragmentMain);
        pipelineDesc->colorAttachments()->object(0)->setPixelFormat(MTL::PixelFormatRGBA16Float);

        bloomCompositePipeline = NS::TransferPtr(device->newRenderPipelineState(pipelineDesc, &error));
        if (!bloomCompositePipeline) {
            throw std::runtime_error(fmt::format(
                "Could not create bloom composite pipeline! Error: {}\n", error->localizedDescription()->utf8String()
            ));
        }

        code->release();
        library->release();
        vertexMain->release();
        fragmentMain->release();
        pipelineDesc->release();
    }

    // ========================================================================
    // Volumetric Fog pipeline (simple height fog)
    // ========================================================================
    {
        auto shaderSrc = readFile("assets/shaders/3d_volumetric_fog.metal");
        auto code = NS::String::string(shaderSrc.data(), NS::StringEncoding::UTF8StringEncoding);
        NS::Error* error = nullptr;
        MTL::Library* library = device->newLibrary(code, nullptr, &error);
        if (!library) {
            fmt::print(
                "Warning: Could not compile volumetric fog shader: {}\n",
                error ? error->localizedDescription()->utf8String() : "unknown error"
            );
        } else {
            auto vertexMain =
                library->newFunction(NS::String::string("volumetricFogVertex", NS::StringEncoding::UTF8StringEncoding));
            auto fragmentMain =
                library->newFunction(NS::String::string("simpleFogFragment", NS::StringEncoding::UTF8StringEncoding));

            if (vertexMain && fragmentMain) {
                auto pipelineDesc = MTL::RenderPipelineDescriptor::alloc()->init();
                pipelineDesc->setVertexFunction(vertexMain);
                pipelineDesc->setFragmentFunction(fragmentMain);
                pipelineDesc->colorAttachments()->object(0)->setPixelFormat(MTL::PixelFormatRGBA16Float);

                fogSimplePipeline = NS::TransferPtr(device->newRenderPipelineState(pipelineDesc, &error));
                if (!fogSimplePipeline) {
                    fmt::print(
                        "Warning: Could not create fog simple pipeline: {}\n",
                        error ? error->localizedDescription()->utf8String() : "unknown error"
                    );
                }

                pipelineDesc->release();
                vertexMain->release();
                fragmentMain->release();
            }
            library->release();
        }
        code->release();
    }

    // ========================================================================
    // Volumetric Cloud render targets (quarter resolution for performance)
    // ========================================================================
    {
        Uint32 cloudWidth = swapchain->drawableSize().width / 4;
        Uint32 cloudHeight = swapchain->drawableSize().height / 4;

        // Quarter-res cloud render target
        MTL::TextureDescriptor* cloudRTDesc = MTL::TextureDescriptor::alloc()->init();
        cloudRTDesc->setTextureType(MTL::TextureType2D);
        cloudRTDesc->setPixelFormat(MTL::PixelFormatRGBA16Float);
        cloudRTDesc->setWidth(cloudWidth);
        cloudRTDesc->setHeight(cloudHeight);
        cloudRTDesc->setUsage(MTL::TextureUsageRenderTarget | MTL::TextureUsageShaderRead);
        cloudRT = NS::TransferPtr(device->newTexture(cloudRTDesc));
        cloudRTDesc->release();

        // History buffer for temporal reprojection (same size as cloudRT)
        MTL::TextureDescriptor* cloudHistoryDesc = MTL::TextureDescriptor::alloc()->init();
        cloudHistoryDesc->setTextureType(MTL::TextureType2D);
        cloudHistoryDesc->setPixelFormat(MTL::PixelFormatRGBA16Float);
        cloudHistoryDesc->setWidth(cloudWidth);
        cloudHistoryDesc->setHeight(cloudHeight);
        cloudHistoryDesc->setUsage(MTL::TextureUsageRenderTarget | MTL::TextureUsageShaderRead);
        cloudHistoryRT = NS::TransferPtr(device->newTexture(cloudHistoryDesc));
        cloudHistoryDesc->release();
    }

    // ========================================================================
    // Volumetric Cloud pipelines
    // ========================================================================
    {
        auto shaderSrc = readFile("assets/shaders/3d_volumetric_clouds.metal");
        auto code = NS::String::string(shaderSrc.data(), NS::StringEncoding::UTF8StringEncoding);
        NS::Error* error = nullptr;
        MTL::Library* library = device->newLibrary(code, nullptr, &error);
        if (!library) {
            fmt::print(
                "Warning: Could not compile volumetric clouds shader: {}\n",
                error ? error->localizedDescription()->utf8String() : "unknown error"
            );
        } else {
            // Low-res cloud rendering pipeline (quarter resolution)
            auto vertexMain =
                library->newFunction(NS::String::string("cloudVertex", NS::StringEncoding::UTF8StringEncoding));
            auto fragmentLowRes =
                library->newFunction(NS::String::string("cloudFragmentLowRes", NS::StringEncoding::UTF8StringEncoding));

            if (vertexMain && fragmentLowRes) {
                auto pipelineDesc = MTL::RenderPipelineDescriptor::alloc()->init();
                pipelineDesc->setVertexFunction(vertexMain);
                pipelineDesc->setFragmentFunction(fragmentLowRes);
                pipelineDesc->colorAttachments()->object(0)->setPixelFormat(MTL::PixelFormatRGBA16Float);

                cloudLowResPipeline = NS::TransferPtr(device->newRenderPipelineState(pipelineDesc, &error));
                if (!cloudLowResPipeline) {
                    fmt::print(
                        "Warning: Could not create cloud low-res pipeline: {}\n",
                        error ? error->localizedDescription()->utf8String() : "unknown error"
                    );
                }
                pipelineDesc->release();
                fragmentLowRes->release();
            }

            // Temporal resolve pipeline
            auto fragmentTemporal =
                library->newFunction(NS::String::string("cloudTemporalResolve", NS::StringEncoding::UTF8StringEncoding)
                );

            if (vertexMain && fragmentTemporal) {
                auto pipelineDesc = MTL::RenderPipelineDescriptor::alloc()->init();
                pipelineDesc->setVertexFunction(vertexMain);
                pipelineDesc->setFragmentFunction(fragmentTemporal);
                pipelineDesc->colorAttachments()->object(0)->setPixelFormat(MTL::PixelFormatRGBA16Float);

                cloudTemporalResolvePipeline = NS::TransferPtr(device->newRenderPipelineState(pipelineDesc, &error));
                if (!cloudTemporalResolvePipeline) {
                    fmt::print(
                        "Warning: Could not create cloud temporal resolve pipeline: {}\n",
                        error ? error->localizedDescription()->utf8String() : "unknown error"
                    );
                }
                pipelineDesc->release();
                fragmentTemporal->release();
            }

            // Upscale and composite pipeline
            auto fragmentComposite =
                library->newFunction(NS::String::string("cloudUpscaleComposite", NS::StringEncoding::UTF8StringEncoding)
                );

            if (vertexMain && fragmentComposite) {
                auto pipelineDesc = MTL::RenderPipelineDescriptor::alloc()->init();
                pipelineDesc->setVertexFunction(vertexMain);
                pipelineDesc->setFragmentFunction(fragmentComposite);
                pipelineDesc->colorAttachments()->object(0)->setPixelFormat(MTL::PixelFormatRGBA16Float);

                cloudCompositePipeline = NS::TransferPtr(device->newRenderPipelineState(pipelineDesc, &error));
                if (!cloudCompositePipeline) {
                    fmt::print(
                        "Warning: Could not create cloud composite pipeline: {}\n",
                        error ? error->localizedDescription()->utf8String() : "unknown error"
                    );
                }
                pipelineDesc->release();
                fragmentComposite->release();
            }

            // Full-res cloud pipeline (fallback/debug)
            auto fragmentFull =
                library->newFunction(NS::String::string("cloudFragment", NS::StringEncoding::UTF8StringEncoding));

            if (vertexMain && fragmentFull) {
                auto pipelineDesc = MTL::RenderPipelineDescriptor::alloc()->init();
                pipelineDesc->setVertexFunction(vertexMain);
                pipelineDesc->setFragmentFunction(fragmentFull);
                pipelineDesc->colorAttachments()->object(0)->setPixelFormat(MTL::PixelFormatRGBA16Float);

                cloudRenderPipeline = NS::TransferPtr(device->newRenderPipelineState(pipelineDesc, &error));
                if (!cloudRenderPipeline) {
                    fmt::print(
                        "Warning: Could not create cloud render pipeline: {}\n",
                        error ? error->localizedDescription()->utf8String() : "unknown error"
                    );
                }
                pipelineDesc->release();
                fragmentFull->release();
            }

            if (vertexMain) vertexMain->release();
            library->release();
        }
        code->release();
    }

    // ========================================================================
    // Sun Flare pipeline
    // ========================================================================
    {
        auto shaderSrc = readFile("assets/shaders/3d_sun_flare.metal");
        auto code = NS::String::string(shaderSrc.data(), NS::StringEncoding::UTF8StringEncoding);
        NS::Error* error = nullptr;
        MTL::Library* library = device->newLibrary(code, nullptr, &error);
        if (!library) {
            fmt::print(
                "Warning: Could not compile sun flare shader: {}\n",
                error ? error->localizedDescription()->utf8String() : "unknown error"
            );
        } else {
            auto vertexMain =
                library->newFunction(NS::String::string("sunFlareVertex", NS::StringEncoding::UTF8StringEncoding));
            auto fragmentMain =
                library->newFunction(NS::String::string("sunFlareFragment", NS::StringEncoding::UTF8StringEncoding));

            if (vertexMain && fragmentMain) {
                auto pipelineDesc = MTL::RenderPipelineDescriptor::alloc()->init();
                pipelineDesc->setVertexFunction(vertexMain);
                pipelineDesc->setFragmentFunction(fragmentMain);
                auto colorAttach = pipelineDesc->colorAttachments()->object(0);
                colorAttach->setPixelFormat(MTL::PixelFormatRGBA16Float);
                // Additive blending: output = src + dst
                colorAttach->setBlendingEnabled(true);
                colorAttach->setSourceRGBBlendFactor(MTL::BlendFactorOne);
                colorAttach->setDestinationRGBBlendFactor(MTL::BlendFactorOne);
                colorAttach->setRgbBlendOperation(MTL::BlendOperationAdd);
                colorAttach->setSourceAlphaBlendFactor(MTL::BlendFactorOne);
                colorAttach->setDestinationAlphaBlendFactor(MTL::BlendFactorZero);
                colorAttach->setAlphaBlendOperation(MTL::BlendOperationAdd);

                sunFlarePipeline = NS::TransferPtr(device->newRenderPipelineState(pipelineDesc, &error));
                if (!sunFlarePipeline) {
                    fmt::print(
                        "Warning: Could not create sun flare pipeline: {}\n",
                        error ? error->localizedDescription()->utf8String() : "unknown error"
                    );
                }

                pipelineDesc->release();
                vertexMain->release();
                fragmentMain->release();
            }
            library->release();
        }
        code->release();
    }

    // ========================================================================
    // DOF (Tilt-Shift) render targets
    // ========================================================================

    // DOF CoC RT (full resolution, RGBA for color + CoC in alpha)
    {
        MTL::TextureDescriptor* dofCoCDesc = MTL::TextureDescriptor::alloc()->init();
        dofCoCDesc->setTextureType(MTL::TextureType2D);
        dofCoCDesc->setPixelFormat(MTL::PixelFormatRGBA16Float);
        dofCoCDesc->setWidth(swapchain->drawableSize().width);
        dofCoCDesc->setHeight(swapchain->drawableSize().height);
        dofCoCDesc->setUsage(MTL::TextureUsageRenderTarget | MTL::TextureUsageShaderRead);
        dofCoCRT = NS::TransferPtr(device->newTexture(dofCoCDesc));
        dofCoCDesc->release();
    }

    // DOF Blur RT (half resolution for performance)
    {
        MTL::TextureDescriptor* dofBlurDesc = MTL::TextureDescriptor::alloc()->init();
        dofBlurDesc->setTextureType(MTL::TextureType2D);
        dofBlurDesc->setPixelFormat(MTL::PixelFormatRGBA16Float);
        dofBlurDesc->setWidth(swapchain->drawableSize().width / 2);
        dofBlurDesc->setHeight(swapchain->drawableSize().height / 2);
        dofBlurDesc->setUsage(MTL::TextureUsageRenderTarget | MTL::TextureUsageShaderRead);
        dofBlurRT = NS::TransferPtr(device->newTexture(dofBlurDesc));
        dofBlurDesc->release();
    }

    // DOF Result RT (full resolution)
    {
        MTL::TextureDescriptor* dofResultDesc = MTL::TextureDescriptor::alloc()->init();
        dofResultDesc->setTextureType(MTL::TextureType2D);
        dofResultDesc->setPixelFormat(MTL::PixelFormatRGBA16Float);
        dofResultDesc->setWidth(swapchain->drawableSize().width);
        dofResultDesc->setHeight(swapchain->drawableSize().height);
        dofResultDesc->setUsage(MTL::TextureUsageRenderTarget | MTL::TextureUsageShaderRead);
        dofResultRT = NS::TransferPtr(device->newTexture(dofResultDesc));
        dofResultDesc->release();
    }

    // ========================================================================
    // DOF (Tilt-Shift) pipelines
    // ========================================================================

    // DOF CoC pipeline
    {
        auto shaderSrc = readFile("assets/shaders/3d_dof_coc.metal");
        auto code = NS::String::string(shaderSrc.data(), NS::StringEncoding::UTF8StringEncoding);
        NS::Error* error = nullptr;
        MTL::Library* library = device->newLibrary(code, nullptr, &error);
        if (!library) {
            throw std::runtime_error(fmt::format(
                "Could not compile DOF CoC shader! Error: {}\n", error->localizedDescription()->utf8String()
            ));
        }

        auto vertexMain =
            library->newFunction(NS::String::string("vertexMain", NS::StringEncoding::UTF8StringEncoding));
        auto fragmentMain =
            library->newFunction(NS::String::string("fragmentMain", NS::StringEncoding::UTF8StringEncoding));

        auto pipelineDesc = MTL::RenderPipelineDescriptor::alloc()->init();
        pipelineDesc->setVertexFunction(vertexMain);
        pipelineDesc->setFragmentFunction(fragmentMain);
        pipelineDesc->colorAttachments()->object(0)->setPixelFormat(MTL::PixelFormatRGBA16Float);

        dofCoCPipeline = NS::TransferPtr(device->newRenderPipelineState(pipelineDesc, &error));
        if (!dofCoCPipeline) {
            throw std::runtime_error(fmt::format(
                "Could not create DOF CoC pipeline! Error: {}\n", error->localizedDescription()->utf8String()
            ));
        }

        code->release();
        library->release();
        vertexMain->release();
        fragmentMain->release();
        pipelineDesc->release();
    }

    // DOF Blur pipeline
    {
        auto shaderSrc = readFile("assets/shaders/3d_dof_blur.metal");
        auto code = NS::String::string(shaderSrc.data(), NS::StringEncoding::UTF8StringEncoding);
        NS::Error* error = nullptr;
        MTL::Library* library = device->newLibrary(code, nullptr, &error);
        if (!library) {
            throw std::runtime_error(fmt::format(
                "Could not compile DOF Blur shader! Error: {}\n", error->localizedDescription()->utf8String()
            ));
        }

        auto vertexMain =
            library->newFunction(NS::String::string("vertexMain", NS::StringEncoding::UTF8StringEncoding));
        auto fragmentMain =
            library->newFunction(NS::String::string("fragmentMain", NS::StringEncoding::UTF8StringEncoding));

        auto pipelineDesc = MTL::RenderPipelineDescriptor::alloc()->init();
        pipelineDesc->setVertexFunction(vertexMain);
        pipelineDesc->setFragmentFunction(fragmentMain);
        pipelineDesc->colorAttachments()->object(0)->setPixelFormat(MTL::PixelFormatRGBA16Float);

        dofBlurPipeline = NS::TransferPtr(device->newRenderPipelineState(pipelineDesc, &error));
        if (!dofBlurPipeline) {
            throw std::runtime_error(fmt::format(
                "Could not create DOF Blur pipeline! Error: {}\n", error->localizedDescription()->utf8String()
            ));
        }

        code->release();
        library->release();
        vertexMain->release();
        fragmentMain->release();
        pipelineDesc->release();
    }

    // DOF Composite pipeline
    {
        auto shaderSrc = readFile("assets/shaders/3d_dof_composite.metal");
        auto code = NS::String::string(shaderSrc.data(), NS::StringEncoding::UTF8StringEncoding);
        NS::Error* error = nullptr;
        MTL::Library* library = device->newLibrary(code, nullptr, &error);
        if (!library) {
            throw std::runtime_error(fmt::format(
                "Could not compile DOF Composite shader! Error: {}\n", error->localizedDescription()->utf8String()
            ));
        }

        auto vertexMain =
            library->newFunction(NS::String::string("vertexMain", NS::StringEncoding::UTF8StringEncoding));
        auto fragmentMain =
            library->newFunction(NS::String::string("fragmentMain", NS::StringEncoding::UTF8StringEncoding));

        auto pipelineDesc = MTL::RenderPipelineDescriptor::alloc()->init();
        pipelineDesc->setVertexFunction(vertexMain);
        pipelineDesc->setFragmentFunction(fragmentMain);
        pipelineDesc->colorAttachments()->object(0)->setPixelFormat(MTL::PixelFormatRGBA16Float);

        dofCompositePipeline = NS::TransferPtr(device->newRenderPipelineState(pipelineDesc, &error));
        if (!dofCompositePipeline) {
            throw std::runtime_error(fmt::format(
                "Could not create DOF Composite pipeline! Error: {}\n", error->localizedDescription()->utf8String()
            ));
        }

        code->release();
        library->release();
        vertexMain->release();
        fragmentMain->release();
        pipelineDesc->release();
    }

    // Create depth stencil states (for depth testing)
    MTL::DepthStencilDescriptor* depthStencilDesc = MTL::DepthStencilDescriptor::alloc()->init();
    depthStencilDesc->setDepthCompareFunction(MTL::CompareFunction::CompareFunctionLessEqual);
    depthStencilDesc->setDepthWriteEnabled(true);
    depthStencilState = NS::TransferPtr(device->newDepthStencilState(depthStencilDesc));
    depthStencilDesc->release();

    // ========================================================================
    // Water rendering resources
    // ========================================================================

    // Create water pipeline with alpha blending
    {
        auto shaderSrc = readFile("assets/shaders/3d_water.metal");
        auto code = NS::String::string(shaderSrc.data(), NS::StringEncoding::UTF8StringEncoding);
        NS::Error* error = nullptr;
        MTL::Library* library = device->newLibrary(code, nullptr, &error);
        if (!library) {
            throw std::runtime_error(
                fmt::format("Could not compile water shader! Error: {}\n", error->localizedDescription()->utf8String())
            );
        }

        auto vertexMain =
            library->newFunction(NS::String::string("vertexMain", NS::StringEncoding::UTF8StringEncoding));
        auto fragmentMain =
            library->newFunction(NS::String::string("fragmentMain", NS::StringEncoding::UTF8StringEncoding));

        auto pipelineDesc = MTL::RenderPipelineDescriptor::alloc()->init();
        pipelineDesc->setVertexFunction(vertexMain);
        pipelineDesc->setFragmentFunction(fragmentMain);

        auto colorAttachment = pipelineDesc->colorAttachments()->object(0);
        colorAttachment->setPixelFormat(MTL::PixelFormat::PixelFormatRGBA16Float);
        colorAttachment->setBlendingEnabled(true);
        colorAttachment->setAlphaBlendOperation(MTL::BlendOperation::BlendOperationAdd);
        colorAttachment->setRgbBlendOperation(MTL::BlendOperation::BlendOperationAdd);
        colorAttachment->setSourceRGBBlendFactor(MTL::BlendFactor::BlendFactorSourceAlpha);
        colorAttachment->setSourceAlphaBlendFactor(MTL::BlendFactor::BlendFactorSourceAlpha);
        colorAttachment->setDestinationRGBBlendFactor(MTL::BlendFactor::BlendFactorOneMinusSourceAlpha);
        colorAttachment->setDestinationAlphaBlendFactor(MTL::BlendFactor::BlendFactorOneMinusSourceAlpha);
        pipelineDesc->setDepthAttachmentPixelFormat(MTL::PixelFormatDepth32Float);
        pipelineDesc->setSampleCount(1);// No MSAA for water pass

        waterPipeline = NS::TransferPtr(device->newRenderPipelineState(pipelineDesc, &error));
        if (!waterPipeline) {
            throw std::runtime_error(
                fmt::format("Could not create water pipeline! Error: {}\n", error->localizedDescription()->utf8String())
            );
        }

        code->release();
        library->release();
        vertexMain->release();
        fragmentMain->release();
        pipelineDesc->release();
    }

    // Create water depth stencil state (depth test but no write for transparency)
    {
        MTL::DepthStencilDescriptor* waterDepthDesc = MTL::DepthStencilDescriptor::alloc()->init();
        waterDepthDesc->setDepthCompareFunction(MTL::CompareFunction::CompareFunctionLessEqual);
        waterDepthDesc->setDepthWriteEnabled(false);// Don't write depth for transparent water
        waterDepthStencilState = NS::TransferPtr(device->newDepthStencilState(waterDepthDesc));
        waterDepthDesc->release();
    }

    // Create water data buffers (triple buffered)
    waterDataBuffers.resize(MAX_FRAMES_IN_FLIGHT);
    for (auto& waterDataBuffer : waterDataBuffers) {
        waterDataBuffer = NS::TransferPtr(device->newBuffer(sizeof(WaterData), MTL::ResourceStorageModeManaged));
    }

    // Create water mesh (100x100 grid with 1.0 unit tiles, 5x5 UV tiling)
    {
        std::vector<WaterVertexData> waterVertices;
        std::vector<Uint32> waterIndices;
        MeshBuilder::buildWaterGrid(100, 100, 1.0f, 5.0f, 5.0f, waterVertices, waterIndices);

        waterVertexBuffer = NS::TransferPtr(
            device->newBuffer(waterVertices.size() * sizeof(WaterVertexData), MTL::ResourceStorageModeManaged)
        );
        memcpy(waterVertexBuffer->contents(), waterVertices.data(), waterVertices.size() * sizeof(WaterVertexData));
        waterVertexBuffer->didModifyRange(NS::Range::Make(0, waterVertexBuffer->length()));

        waterIndexBuffer =
            NS::TransferPtr(device->newBuffer(waterIndices.size() * sizeof(Uint32), MTL::ResourceStorageModeManaged));
        memcpy(waterIndexBuffer->contents(), waterIndices.data(), waterIndices.size() * sizeof(Uint32));
        waterIndexBuffer->didModifyRange(NS::Range::Make(0, waterIndexBuffer->length()));

        waterIndexCount = static_cast<Uint32>(waterIndices.size());
    }

    // Initialize default water transform (positioned above floor in Sponza)
    waterTransform.position = glm::vec3(0.0f, 0.5f, 0.0f);// y=0.5 to be above the floor
    waterTransform.scale = glm::vec3(1.0f, 1.0f, 1.0f);

    // Initialize default water settings
    waterSettings.modelMatrix = glm::mat4(1.0f);
    waterSettings.surfaceColor = glm::vec4(0.465f, 0.797f, 0.991f, 1.0f);
    waterSettings.refractionColor = glm::vec4(0.003f, 0.599f, 0.812f, 1.0f);
    // SSR settings: x=step size, y=max steps (0 to disable), z=refinement steps, w=distance factor
    waterSettings.ssrSettings = glm::vec4(0.5f, 0.0f, 10.0f, 20.0f);// Set y=0 to disable SSR
    waterSettings.normalMapScroll = glm::vec4(1.0f, 0.0f, 0.0f, 1.0f);
    waterSettings.normalMapScrollSpeed = glm::vec2(0.01f, 0.01f);
    waterSettings.refractionDistortionFactor = 0.04f;
    waterSettings.refractionHeightFactor = 2.5f;
    waterSettings.refractionDistanceFactor = 15.0f;
    waterSettings.depthSofteningDistance = 0.5f;
    waterSettings.foamHeightStart = 0.8f;
    waterSettings.foamFadeDistance = 0.4f;
    waterSettings.foamTiling = 2.0f;
    waterSettings.foamAngleExponent = 80.0f;
    waterSettings.roughness = 0.08f;
    waterSettings.reflectance = 0.55f;
    waterSettings.specIntensity = 125.0f;
    waterSettings.foamBrightness = 4.0f;
    // waterSettings.tessellationFactor = 7.0f;
    waterSettings.dampeningFactor = 5.0f;
    waterSettings.waveCount = 2;

    // Wave 1
    waterSettings.waves[0].direction = glm::vec3(0.3f, 0.0f, -0.7f);
    waterSettings.waves[0].steepness = 1.79f;
    waterSettings.waves[0].waveLength = 3.75f;
    waterSettings.waves[0].amplitude = 0.85f;
    waterSettings.waves[0].speed = 1.21f;

    // Wave 2
    waterSettings.waves[1].direction = glm::vec3(0.5f, 0.0f, -0.2f);
    waterSettings.waves[1].steepness = 1.79f;
    waterSettings.waves[1].waveLength = 4.1f;
    waterSettings.waves[1].amplitude = 0.52f;
    waterSettings.waves[1].speed = 1.03f;

    // Create placeholder water textures (procedural normal maps and noise)
    // Water normal map 1 - a simple procedural normal texture
    {
        const Uint32 texSize = 256;
        std::vector<Uint8> normalData(texSize * texSize * 4);
        for (Uint32 y = 0; y < texSize; ++y) {
            for (Uint32 x = 0; x < texSize; ++x) {
                float fx = static_cast<float>(x) / texSize * 6.28f;
                float fy = static_cast<float>(y) / texSize * 6.28f;
                float nx = sin(fx * 2.0f + fy) * 0.5f + 0.5f;
                float ny = sin(fy * 2.0f + fx * 0.5f) * 0.5f + 0.5f;
                float nz = 1.0f;
                glm::vec3 n = glm::normalize(glm::vec3((nx - 0.5f) * 0.3f, (ny - 0.5f) * 0.3f, nz));
                Uint32 idx = (y * texSize + x) * 4;
                normalData[idx + 0] = static_cast<Uint8>((n.x * 0.5f + 0.5f) * 255);
                normalData[idx + 1] = static_cast<Uint8>((n.y * 0.5f + 0.5f) * 255);
                normalData[idx + 2] = static_cast<Uint8>((n.z * 0.5f + 0.5f) * 255);
                normalData[idx + 3] = 255;
            }
        }
        auto img = std::make_shared<Image>();
        img->uri = "procedural_water_normal1";
        img->width = texSize;
        img->height = texSize;
        img->channelCount = 4;
        img->byteArray = normalData;
        waterNormalMap1 = createTexture(img);
    }

    // Water normal map 2 - different pattern
    {
        const Uint32 texSize = 256;
        std::vector<Uint8> normalData(texSize * texSize * 4);
        for (Uint32 y = 0; y < texSize; ++y) {
            for (Uint32 x = 0; x < texSize; ++x) {
                float fx = static_cast<float>(x) / texSize * 6.28f;
                float fy = static_cast<float>(y) / texSize * 6.28f;
                float nx = cos(fx * 3.0f - fy * 0.5f) * 0.5f + 0.5f;
                float ny = cos(fy * 3.0f + fx * 0.7f) * 0.5f + 0.5f;
                float nz = 1.0f;
                glm::vec3 n = glm::normalize(glm::vec3((nx - 0.5f) * 0.25f, (ny - 0.5f) * 0.25f, nz));
                Uint32 idx = (y * texSize + x) * 4;
                normalData[idx + 0] = static_cast<Uint8>((n.x * 0.5f + 0.5f) * 255);
                normalData[idx + 1] = static_cast<Uint8>((n.y * 0.5f + 0.5f) * 255);
                normalData[idx + 2] = static_cast<Uint8>((n.z * 0.5f + 0.5f) * 255);
                normalData[idx + 3] = 255;
            }
        }
        auto img = std::make_shared<Image>();
        img->uri = "procedural_water_normal2";
        img->width = texSize;
        img->height = texSize;
        img->channelCount = 4;
        img->byteArray = normalData;
        waterNormalMap2 = createTexture(img);
    }

    // Water foam texture - white with noise pattern
    {
        const Uint32 texSize = 256;
        std::vector<Uint8> foamData(texSize * texSize * 4);
        for (Uint32 y = 0; y < texSize; ++y) {
            for (Uint32 x = 0; x < texSize; ++x) {
                float fx = static_cast<float>(x) / texSize;
                float fy = static_cast<float>(y) / texSize;
                float noise = (sin(fx * 50.0f) * cos(fy * 50.0f) + 1.0f) * 0.5f;
                noise *= (sin(fx * 30.0f + fy * 20.0f) + 1.0f) * 0.5f;
                Uint8 v = static_cast<Uint8>(noise * 200 + 55);
                Uint32 idx = (y * texSize + x) * 4;
                foamData[idx + 0] = v;
                foamData[idx + 1] = v;
                foamData[idx + 2] = v;
                foamData[idx + 3] = 255;
            }
        }
        auto img = std::make_shared<Image>();
        img->uri = "procedural_water_foam";
        img->width = texSize;
        img->height = texSize;
        img->channelCount = 4;
        img->byteArray = foamData;
        waterFoamMap = createTexture(img);
    }

    // Water noise texture - Perlin-like noise pattern
    {
        const Uint32 texSize = 256;
        std::vector<Uint8> noiseData(texSize * texSize * 4);
        for (Uint32 y = 0; y < texSize; ++y) {
            for (Uint32 x = 0; x < texSize; ++x) {
                float fx = static_cast<float>(x) / texSize;
                float fy = static_cast<float>(y) / texSize;
                float noise = 0.0f;
                noise += (sin(fx * 20.0f) * cos(fy * 20.0f) + 1.0f) * 0.25f;
                noise += (sin(fx * 40.0f + 0.3f) * cos(fy * 40.0f + 0.7f) + 1.0f) * 0.125f;
                noise += (sin(fx * 80.0f + 1.5f) * cos(fy * 80.0f + 2.1f) + 1.0f) * 0.0625f;
                noise = glm::clamp(noise, 0.0f, 1.0f);
                Uint8 v = static_cast<Uint8>(noise * 255);
                Uint32 idx = (y * texSize + x) * 4;
                noiseData[idx + 0] = v;
                noiseData[idx + 1] = v;
                noiseData[idx + 2] = v;
                noiseData[idx + 3] = 255;
            }
        }
        auto img = std::make_shared<Image>();
        img->uri = "procedural_water_noise";
        img->width = texSize;
        img->height = texSize;
        img->channelCount = 4;
        img->byteArray = noiseData;
        waterNoiseMap = createTexture(img);
    }

    // Create placeholder environment cube map (simple gradient sky)
    {
        const Uint32 faceSize = 64;
        MTL::TextureDescriptor* cubeDesc = MTL::TextureDescriptor::alloc()->init();
        cubeDesc->setTextureType(MTL::TextureTypeCube);
        cubeDesc->setPixelFormat(MTL::PixelFormatRGBA8Unorm);
        cubeDesc->setWidth(faceSize);
        cubeDesc->setHeight(faceSize);
        cubeDesc->setUsage(MTL::TextureUsageShaderRead);

        environmentCubeMap = NS::TransferPtr(device->newTexture(cubeDesc));

        std::vector<Uint8> faceData(faceSize * faceSize * 4);
        for (Uint32 face = 0; face < 6; ++face) {
            for (Uint32 y = 0; y < faceSize; ++y) {
                for (Uint32 x = 0; x < faceSize; ++x) {
                    float t = static_cast<float>(y) / faceSize;
                    Uint8 r = static_cast<Uint8>((0.4f + t * 0.3f) * 255);
                    Uint8 g = static_cast<Uint8>((0.6f + t * 0.2f) * 255);
                    Uint8 b = static_cast<Uint8>((0.8f + t * 0.1f) * 255);
                    Uint32 idx = (y * faceSize + x) * 4;
                    faceData[idx + 0] = r;
                    faceData[idx + 1] = g;
                    faceData[idx + 2] = b;
                    faceData[idx + 3] = 255;
                }
            }
            environmentCubeMap->replaceRegion(
                MTL::Region(0, 0, 0, faceSize, faceSize, 1), 0, face, faceData.data(), faceSize * 4, 0
            );
        }

        cubeDesc->release();
    }

    // ========================================================================
    // Particle system initialization
    // ========================================================================

    // Create particle compute pipelines
    {
        NS::Error* error = nullptr;
        auto shaderSrc = readFile("assets/shaders/3d_particle.metal");
        auto code = NS::String::string(shaderSrc.data(), NS::StringEncoding::UTF8StringEncoding);
        MTL::CompileOptions* options = nullptr;
        auto library = NS::TransferPtr(device->newLibrary(code, options, &error));

        if (!library) {
            fmt::print("Failed to compile particle compute shader: {}\n", error->localizedDescription()->utf8String());
        } else {
            // Create force pipeline
            auto forceFuncName = NS::String::string("particleForce", NS::StringEncoding::UTF8StringEncoding);
            auto forceFunc = library->newFunction(forceFuncName);
            if (forceFunc) {
                particleForcePipeline = NS::TransferPtr(device->newComputePipelineState(forceFunc, &error));
                forceFunc->release();
            }

            // Create integrate pipeline
            auto integrateFuncName = NS::String::string("particleIntegrate", NS::StringEncoding::UTF8StringEncoding);
            auto integrateFunc = library->newFunction(integrateFuncName);
            if (integrateFunc) {
                particleIntegratePipeline = NS::TransferPtr(device->newComputePipelineState(integrateFunc, &error));
                integrateFunc->release();
            }
        }
        code->release();
    }

    // Create particle render pipeline - compile from source file
    {
        NS::Error* error = nullptr;
        auto shaderSource = readFile("assets/shaders/3d_particle.metal");
        auto source = NS::String::string(shaderSource.c_str(), NS::UTF8StringEncoding);
        auto options = NS::TransferPtr(MTL::CompileOptions::alloc()->init());
        auto library = NS::TransferPtr(device->newLibrary(source, options.get(), &error));

        if (!library) {
            fmt::print("Failed to compile particle render shader: {}\n", error->localizedDescription()->utf8String());
        } else {
            auto vertexFunc =
                NS::TransferPtr(library->newFunction(NS::String::string("particleVertex", NS::UTF8StringEncoding)));
            auto fragFunc =
                NS::TransferPtr(library->newFunction(NS::String::string("particleFragment", NS::UTF8StringEncoding)));

            if (!vertexFunc || !fragFunc) {
                fmt::print("Failed to find particle vertex/fragment functions\n");
            } else {
                auto pipelineDesc = NS::TransferPtr(MTL::RenderPipelineDescriptor::alloc()->init());
                pipelineDesc->setVertexFunction(vertexFunc.get());
                pipelineDesc->setFragmentFunction(fragFunc.get());

                // Color attachment with additive blending
                auto colorAttachment = pipelineDesc->colorAttachments()->object(0);
                colorAttachment->setPixelFormat(MTL::PixelFormatRGBA16Float);
                colorAttachment->setBlendingEnabled(true);
                colorAttachment->setSourceRGBBlendFactor(MTL::BlendFactorOne);
                colorAttachment->setDestinationRGBBlendFactor(MTL::BlendFactorOne);
                colorAttachment->setRgbBlendOperation(MTL::BlendOperationAdd);
                colorAttachment->setSourceAlphaBlendFactor(MTL::BlendFactorOne);
                colorAttachment->setDestinationAlphaBlendFactor(MTL::BlendFactorZero);
                colorAttachment->setAlphaBlendOperation(MTL::BlendOperationAdd);

                pipelineDesc->setDepthAttachmentPixelFormat(MTL::PixelFormatDepth32Float);

                particleRenderPipeline = NS::TransferPtr(device->newRenderPipelineState(pipelineDesc.get(), &error));
                if (error) {
                    fmt::print(
                        "Failed to create particle render pipeline: {}\n", error->localizedDescription()->utf8String()
                    );
                }
            }
        }

        // Create depth stencil state (depth test enabled, write disabled)
        auto depthDesc = MTL::DepthStencilDescriptor::alloc()->init();
        depthDesc->setDepthCompareFunction(MTL::CompareFunctionLess);
        depthDesc->setDepthWriteEnabled(false);
        particleDepthStencilState = NS::TransferPtr(device->newDepthStencilState(depthDesc));
        depthDesc->release();
    }

    // Create particle buffers
    // Single particle buffer for persistent state (not triple-buffered)
    size_t particleBufferSize = sizeof(GPUParticle) * MAX_PARTICLES;
    particleBuffer = NS::TransferPtr(device->newBuffer(particleBufferSize, MTL::ResourceStorageModeShared));

    // Per-frame uniform buffers (triple-buffered)
    particleSimParamsBuffers.resize(MAX_FRAMES_IN_FLIGHT);
    particleAttractorBuffers.resize(MAX_FRAMES_IN_FLIGHT);
    for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        particleSimParamsBuffers[i] =
            NS::TransferPtr(device->newBuffer(sizeof(ParticleSimulationParams), MTL::ResourceStorageModeShared));
        particleAttractorBuffers[i] =
            NS::TransferPtr(device->newBuffer(sizeof(ParticleAttractorData), MTL::ResourceStorageModeShared));
    }

    // Initialize particles with random positions and colors
    {
        std::srand(static_cast<unsigned>(std::time(nullptr)));

        GPUParticle* particles = reinterpret_cast<GPUParticle*>(particleBuffer->contents());
        for (size_t i = 0; i < MAX_PARTICLES; i++) {
            // Minimum radius of 0.5 to avoid particles at origin
            float r = 0.5f + std::sqrt(static_cast<float>(std::rand()) / RAND_MAX) * 4.5f;
            float theta = static_cast<float>(std::rand()) / RAND_MAX * 2.0f * 3.14159265f;
            float phi = static_cast<float>(std::rand()) / RAND_MAX * 3.14159265f;

            particles[i].position =
                glm::vec3(r * std::sin(phi) * std::cos(theta), r * std::sin(phi) * std::sin(theta), r * std::cos(phi));

            // Initialize tangential velocity for orbital motion
            glm::vec3 tangent = glm::normalize(glm::cross(particles[i].position, glm::vec3(0.0f, 1.0f, 0.0f)));
            if (glm::length(tangent) < 0.001f) {
                tangent = glm::vec3(1.0f, 0.0f, 0.0f);
            }
            // Increase initial velocity for more dynamic motion (was 0.5)
            // Velocity inversely proportional to radius for stable orbits
            particles[i].velocity = tangent * (1.5f / std::sqrt(r + 0.1f));
            particles[i].force = glm::vec3(0.0f);

            float brightness = 1.0f - (r / 5.0f);

            // "Nocturne" palette - mysterious, elegant purple-blue gradient
            // Perfect for piano atmosphere: deep purple → indigo → electric blue
            glm::vec3 a = glm::vec3(0.25f, 0.25f, 0.6f);// Base: royal blue
            glm::vec3 b = glm::vec3(0.35f, 0.3f, 0.4f);// Amplitude: purple-blue dominant
            glm::vec3 c = glm::vec3(0.8f, 0.9f, 1.0f);// Frequency: blue channel most active
            glm::vec3 d = glm::vec3(0.7f, 0.65f, 0.5f);// Phase: starts from purple

            glm::vec3 color = a + b * glm::cos(6.28318f * (c * brightness + d));
            // Clamp color to [0, 1] to prevent negative values and oversaturation
            color = glm::clamp(color, 0.0f, 1.0f);
            particles[i].color = glm::vec4(color, 1.0f);
        }
    }

    fmt::print("Particle system initialized with {} particles\n", MAX_PARTICLES);
}

auto Renderer_Metal::stage(std::shared_ptr<Scene> scene) -> void {
    ZoneScoped;

    // Lights
    directionalLightBuffer = NS::TransferPtr(
        device->newBuffer(scene->directionalLights.size() * sizeof(DirectionalLight), MTL::ResourceStorageModeManaged)
    );
    memcpy(
        directionalLightBuffer->contents(),
        scene->directionalLights.data(),
        scene->directionalLights.size() * sizeof(DirectionalLight)
    );
    directionalLightBuffer->didModifyRange(NS::Range::Make(0, directionalLightBuffer->length()));

    pointLightBuffer = NS::TransferPtr(
        device->newBuffer(scene->pointLights.size() * sizeof(PointLight), MTL::ResourceStorageModeManaged)
    );
    memcpy(pointLightBuffer->contents(), scene->pointLights.data(), scene->pointLights.size() * sizeof(PointLight));
    pointLightBuffer->didModifyRange(NS::Range::Make(0, pointLightBuffer->length()));

    // Textures
    for (auto& img : scene->images) {
        img->texture = createTexture(img);
    }

    // Pipelines & materials
    if (scene->materials.empty()) {
        // TODO: create default material
    }
    for (auto& mat : scene->materials) {
        // pipelines[mat->pipeline] = createPipeline();
        materialIDs[mat] = nextMaterialID++;
    }
    materialDataBuffer = NS::TransferPtr(
        device->newBuffer(scene->materials.size() * sizeof(MaterialData), MTL::ResourceStorageModeManaged)
    );

    // Buffers
    scene->vertexBuffer = createVertexBuffer(scene->vertices);
    scene->indexBuffer = createIndexBuffer(scene->indices);

    auto cmd = queue->commandBuffer();

    const std::function<void(const std::shared_ptr<Node>&)> stageNode = [&](const std::shared_ptr<Node>& node) {
        if (node->meshGroup) {
            for (auto& mesh : node->meshGroup->meshes) {
                // mesh->vbos.push_back(createVertexBuffer(mesh->vertices));
                // mesh->ebo = createIndexBuffer(mesh->indices);

                auto geomDesc = NS::TransferPtr(MTL::AccelerationStructureTriangleGeometryDescriptor::alloc()->init());
                geomDesc->setVertexBuffer(getBuffer(scene->vertexBuffer).get());
                geomDesc->setVertexStride(sizeof(VertexData));
                geomDesc->setVertexFormat(MTL::AttributeFormatFloat3);
                geomDesc->setVertexBufferOffset(
                    mesh->vertexOffset * sizeof(VertexData) + offsetof(VertexData, position)
                );
                geomDesc->setIndexBuffer(getBuffer(scene->indexBuffer).get());
                geomDesc->setIndexType(MTL::IndexTypeUInt32);
                geomDesc->setIndexBufferOffset(mesh->indexOffset * sizeof(Uint32));
                geomDesc->setTriangleCount(mesh->indexCount / 3);
                geomDesc->setOpaque(true);

                auto accelDesc = NS::TransferPtr(MTL::PrimitiveAccelerationStructureDescriptor::alloc()->init());
                NS::Object* descriptors[] = { geomDesc.get() };
                auto geomArray = NS::TransferPtr(NS::Array::array(descriptors, 1));
                accelDesc->setGeometryDescriptors(geomArray.get());

                auto accelSizes = device->accelerationStructureSizes(accelDesc.get());
                auto accelStruct =
                    NS::TransferPtr(device->newAccelerationStructure(accelSizes.accelerationStructureSize));
                auto scratchBuffer = NS::TransferPtr(
                    device->newBuffer(accelSizes.buildScratchBufferSize, MTL::ResourceStorageModePrivate)
                );

                auto encoder = cmd->accelerationStructureCommandEncoder();
                encoder->buildAccelerationStructure(accelStruct.get(), accelDesc.get(), scratchBuffer.get(), 0);
                encoder->endEncoding();

                BLASs.push_back(accelStruct);

                mesh->materialID = materialIDs[mesh->material];
                mesh->instanceID = nextInstanceID++;
            }
        }
        for (const auto& child : node->children) {
            stageNode(child);
        }
    };
    for (auto& node : scene->nodes) {
        stageNode(node);
    }

    cmd->commit();
}

auto Renderer_Metal::draw(std::shared_ptr<Scene> scene, Camera& camera) -> void {
    ZoneScoped;
    FrameMark;

    // Get drawable (autoreleased, will be managed by system AutoreleasePool)
    auto surface = swapchain->nextDrawable();
    if (!surface) {
        return;
    }

    // ==========================================================================
    // Prepare frame data
    // ==========================================================================
    auto time = (float)SDL_GetTicks() / 1000.0f;

    FrameData* frameData = reinterpret_cast<FrameData*>(frameDataBuffers[currentFrameInFlight]->contents());
    frameData->frameNumber = frameNumber;
    frameData->time = time;
    frameData->deltaTime = 0.016f;// TODO:
    frameDataBuffers[currentFrameInFlight]->didModifyRange(
        NS::Range::Make(0, frameDataBuffers[currentFrameInFlight]->length())
    );

    float near = camera.near();
    float far = camera.far();
    glm::vec3 camPos = camera.getEye();
    glm::mat4 proj = camera.getProjMatrix();
    glm::mat4 view = camera.getViewMatrix();
    glm::mat4 invProj = glm::inverse(proj);
    glm::mat4 invView = glm::inverse(view);
    CameraData* cameraData = reinterpret_cast<CameraData*>(cameraDataBuffers[currentFrameInFlight]->contents());
    cameraData->proj = proj;
    cameraData->view = view;
    cameraData->invProj = invProj;
    cameraData->invView = invView;
    cameraData->near = near;
    cameraData->far = far;
    cameraData->position = camPos;
    cameraDataBuffers[currentFrameInFlight]->didModifyRange(
        NS::Range::Make(0, cameraDataBuffers[currentFrameInFlight]->length())
    );

    DirectionalLight* dirLights = reinterpret_cast<DirectionalLight*>(directionalLightBuffer->contents());
    for (size_t i = 0; i < scene->directionalLights.size(); ++i) {
        dirLights[i].direction = scene->directionalLights[i].direction;
        dirLights[i].color = scene->directionalLights[i].color;
        dirLights[i].intensity = scene->directionalLights[i].intensity;
    }
    directionalLightBuffer->didModifyRange(NS::Range::Make(0, directionalLightBuffer->length()));

    AtmosphereData* atmosphereData = reinterpret_cast<AtmosphereData*>(atmosphereDataBuffer->contents());
    if (!scene->directionalLights.empty()) {
        const auto& sunLight = scene->directionalLights[0];
        atmosphereData->sunDirection = -glm::normalize(sunLight.direction);
        atmosphereData->sunColor = sunLight.color;
        atmosphereData->sunIntensity = sunLight.intensity;
    }

    PointLight* pointLights = reinterpret_cast<PointLight*>(pointLightBuffer->contents());
    for (size_t i = 0; i < scene->pointLights.size(); ++i) {
        pointLights[i].position = scene->pointLights[i].position;
        pointLights[i].color = scene->pointLights[i].color;
        pointLights[i].intensity = scene->pointLights[i].intensity;
        pointLights[i].radius = scene->pointLights[i].radius;
    }
    pointLightBuffer->didModifyRange(NS::Range::Make(0, pointLightBuffer->length()));

    MaterialData* materialData = reinterpret_cast<MaterialData*>(materialDataBuffer->contents());
    for (size_t i = 0; i < scene->materials.size(); ++i) {
        const auto& mat = scene->materials[i];
        materialData[i] = MaterialData{ .baseColorFactor = mat->baseColorFactor,
                                        .normalScale = mat->normalScale,
                                        .metallicFactor = mat->metallicFactor,
                                        .roughnessFactor = mat->roughnessFactor,
                                        .occlusionStrength = mat->occlusionStrength,
                                        .emissiveFactor = mat->emissiveFactor,
                                        .emissiveStrength = mat->emissiveStrength,
                                        .subsurface = mat->subsurface,
                                        .specular = mat->specular,
                                        .specularTint = mat->specularTint,
                                        .anisotropic = mat->anisotropic,
                                        .sheen = mat->sheen,
                                        .sheenTint = mat->sheenTint,
                                        .clearcoat = mat->clearcoat,
                                        .clearcoatGloss = mat->clearcoatGloss,
                                        .usePrototypeUV = mat->usePrototypeUV ? 1.0f : 0.0f };
    }
    materialDataBuffer->didModifyRange(NS::Range::Make(0, materialDataBuffer->length()));

    // Update instance data
    instances.clear();
    accelInstances.clear();
    instanceBatches.clear();
    const std::function<void(const std::shared_ptr<Node>&)> updateNode = [&](const std::shared_ptr<Node>& node) {
        if (node->meshGroup) {
            const glm::mat4& transform = node->worldTransform;
            for (const auto& mesh : node->meshGroup->meshes) {
                instances.push_back({
                    .model = transform,
                    .color = glm::vec4(1.0f, 0.0f, 0.0f, 1.0f),
                    .vertexOffset = mesh->vertexOffset,
                    .indexOffset = mesh->indexOffset,
                    .vertexCount = mesh->vertexCount,
                    .indexCount = mesh->indexCount,
                    .materialID = mesh->materialID,
                    .primitiveMode = mesh->primitiveMode,
                    .AABBMin = mesh->worldAABBMin,
                    .AABBMax = mesh->worldAABBMax,
                });
                MTL::AccelerationStructureInstanceDescriptor accelInstanceDesc;
                for (int i = 0; i < 4; ++i) {
                    for (int j = 0; j < 3; ++j) {
                        accelInstanceDesc.transformationMatrix.columns[i][j] = transform[i][j];
                    }
                }
                accelInstanceDesc.accelerationStructureIndex = mesh->instanceID;
                accelInstanceDesc.mask = 0xFF;
                accelInstances.push_back(accelInstanceDesc);
                if (!mesh->material) {
                    fmt::print("No material found for mesh in mesh group {}\n", node->meshGroup->name);
                    continue;
                }
                instanceBatches[mesh->material].push_back(mesh);
            }
        }
        for (const auto& child : node->children) {
            updateNode(child);
        }
    };
    for (const auto& node : scene->nodes) {
        updateNode(node);
    }
    if (instances.size() > MAX_INSTANCES) {// TODO: reallocate when needed
        fmt::print("Warning: Instance count ({}) exceeds MAX_INSTANCES ({})\n", instances.size(), MAX_INSTANCES);
    }
    // TODO: avoid updating the entire instance data buffer every frame
    memcpy(
        instanceDataBuffers[currentFrameInFlight]->contents(), instances.data(), instances.size() * sizeof(InstanceData)
    );
    instanceDataBuffers[currentFrameInFlight]->didModifyRange(
        NS::Range::Make(0, instanceDataBuffers[currentFrameInFlight]->length())
    );
    memcpy(
        accelInstanceBuffers[currentFrameInFlight]->contents(),
        accelInstances.data(),
        accelInstances.size() * sizeof(MTL::AccelerationStructureInstanceDescriptor)
    );
    accelInstanceBuffers[currentFrameInFlight]->didModifyRange(
        NS::Range::Make(0, accelInstanceBuffers[currentFrameInFlight]->length())
    );

    // ==========================================================================
    // Set up rendering context for passes
    // ==========================================================================
    auto cmd = queue->commandBuffer();
    currentCommandBuffer = cmd;
    currentScene = scene;
    currentCamera = &camera;
    currentDrawable = surface;
    drawCount = 0;

    // ==========================================================================
    // Initialize RmlUI if not already initialized (delayed initialization)
    // ==========================================================================
    auto* engineCore = Vapor::EngineCore::Get();
    if (engineCore) {
        auto* rmluiManager = engineCore->getRmlUiManager();
        if (!rmluiManager) {
            // Initialize RmlUI with current window size
            int width = static_cast<int>(surface->texture()->width());
            int height = static_cast<int>(surface->texture()->height());
            if (engineCore->initRmlUI(width, height)) {
                // Initialize renderer UI support (sets RenderInterface and finalizes RmlUI)
                initUI();
            }
        }
    }

    // ==========================================================================
    // Build ImGui UI (before ImGuiPass executes)
    // ==========================================================================
    // Create temporary render pass descriptor for ImGui initialization
    auto imguiPassDesc = NS::TransferPtr(MTL::RenderPassDescriptor::renderPassDescriptor());
    auto imguiPassColorRT = imguiPassDesc->colorAttachments()->object(0);
    imguiPassColorRT->setTexture(surface->texture());

    ImGui_ImplMetal_NewFrame(imguiPassDesc.get());
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();

    // ImGui::DockSpaceOverViewport();

    if (ImGui::CollapsingHeader("Graphics", ImGuiTreeNodeFlags_DefaultOpen)) {
        // ImGui::Text("Frame rate: %.3f ms/frame (%.1f FPS)", 1000.0f * deltaTime, 1.0f / deltaTime);
        ImGui::Text(
            "Average frame rate: %.3f ms/frame (%.1f FPS)", 1000.0f / ImGui::GetIO().Framerate, ImGui::GetIO().Framerate
        );
        ImGui::ColorEdit3("Clear color", (float*)&clearColor);

        ImGui::Separator();

        if (ImGui::TreeNode("RTs")) {
            ImGui::Separator();
            if (ImGui::TreeNode(fmt::format("Scene Color RT").c_str())) {
                ImGui::Image((ImTextureID)(intptr_t)colorRT.get(), ImVec2(64, 64));
                ImGui::TreePop();
            }
            if (ImGui::TreeNode(fmt::format("Scene Depth RT").c_str())) {
                ImGui::Image((ImTextureID)(intptr_t)depthStencilRT.get(), ImVec2(64, 64));
                ImGui::TreePop();
            }
            if (ImGui::TreeNode(fmt::format("Raytraced Shadow").c_str())) {
                ImGui::Image((ImTextureID)(intptr_t)shadowRT.get(), ImVec2(64, 64));
                ImGui::TreePop();
            }
            if (ImGui::TreeNode(fmt::format("Raytraced AO").c_str())) {
                ImGui::Image((ImTextureID)(intptr_t)aoRT.get(), ImVec2(64, 64));
                ImGui::TreePop();
            }
            if (ImGui::TreeNode(fmt::format("Scene Normal RT").c_str())) {
                ImGui::Image((ImTextureID)(intptr_t)normalRT.get(), ImVec2(64, 64));
                ImGui::TreePop();
            }
            if (lightScatteringRT) {
                if (ImGui::TreeNode(fmt::format("Light Scattering RT").c_str())) {
                    ImGui::Image((ImTextureID)(intptr_t)lightScatteringRT.get(), ImVec2(64, 64));
                    ImGui::TreePop();
                }
            }
            ImGui::TreePop();
        }

        if (ImGui::TreeNode("Scene Materials")) {
            ImGui::Separator();
            for (auto m : scene->materials) {
                if (!m) {
                    continue;
                }
                ImGui::PushID(m.get());
                if (ImGui::TreeNode(fmt::format("Mat #{}", m->name).c_str())) {
                    // TODO: show error image if texture is not uploaded
                    if (m->albedoMap) {
                        ImGui::Text("Albedo Map");
                        ImGui::Image((ImTextureID)(intptr_t)getTexture(m->albedoMap->texture).get(), ImVec2(64, 64));
                    }
                    if (m->normalMap) {
                        ImGui::Text("Normal Map");
                        ImGui::Image((ImTextureID)(intptr_t)getTexture(m->normalMap->texture).get(), ImVec2(64, 64));
                    }
                    if (m->metallicMap) {
                        ImGui::Text("Metallic Map");
                        ImGui::Image((ImTextureID)(intptr_t)getTexture(m->metallicMap->texture).get(), ImVec2(64, 64));
                    }
                    if (m->roughnessMap) {
                        ImGui::Text("Roughness Map");
                        ImGui::Image((ImTextureID)(intptr_t)getTexture(m->roughnessMap->texture).get(), ImVec2(64, 64));
                    }
                    if (m->occlusionMap) {
                        ImGui::Text("Occlusion Map");
                        ImGui::Image((ImTextureID)(intptr_t)getTexture(m->occlusionMap->texture).get(), ImVec2(64, 64));
                    }
                    if (m->emissiveMap) {
                        ImGui::Text("Emissive Map");
                        ImGui::Image((ImTextureID)(intptr_t)getTexture(m->emissiveMap->texture).get(), ImVec2(64, 64));
                    }
                    ImGui::ColorEdit4("Base Color Factor", (float*)&m->baseColorFactor);
                    ImGui::DragFloat("Normal Scale", &m->normalScale, .05f, 0.0f, 5.0f);
                    ImGui::DragFloat("Roughness Factor", &m->roughnessFactor, .05f, 0.0f, 5.0f);
                    ImGui::DragFloat("Metallic Factor", &m->metallicFactor, .05f, 0.0f, 5.0f);
                    ImGui::DragFloat("Occlusion Strength", &m->occlusionStrength, .05f, 0.0f, 5.0f);
                    ImGui::ColorEdit3("Emissive Color Factor", (float*)&m->emissiveFactor);
                    ImGui::DragFloat("Emissive Strength", &m->emissiveStrength, .05f, 0.0f, 5.0f);
                    ImGui::DragFloat("Subsurface", &m->subsurface, .01f, 0.0f, 1.0f);
                    ImGui::DragFloat("Specular", &m->specular, .01f, 0.0f, 1.0f);
                    ImGui::DragFloat("Specular Tint", &m->specularTint, .01f, 0.0f, 1.0f);
                    ImGui::DragFloat("Anisotropic", &m->anisotropic, .01f, 0.0f, 1.0f);
                    ImGui::DragFloat("Sheen", &m->sheen, .01f, 0.0f, 1.0f);
                    ImGui::DragFloat("Sheen Tint", &m->sheenTint, .01f, 0.0f, 1.0f);
                    ImGui::DragFloat("Clearcoat", &m->clearcoat, .01f, 0.0f, 1.0f);
                    ImGui::DragFloat("Clearcoat Gloss", &m->clearcoatGloss, .01f, 0.0f, 1.0f);
                    ImGui::TreePop();
                }
                ImGui::PopID();
            }
            ImGui::TreePop();
        }

        if (ImGui::TreeNode("Scene Lights")) {
            ImGui::Separator();
            for (auto& l : scene->directionalLights) {
                ImGui::Text("Directional Light");
                ImGui::PushID(&l);
                ImGui::DragFloat3("Direction", (float*)&l.direction, 0.1f);
                ImGui::ColorEdit3("Color", (float*)&l.color);
                ImGui::DragFloat("Intensity", &l.intensity, 0.1f, 0.0001f);
                ImGui::PopID();
            }
            for (auto& l : scene->pointLights) {
                ImGui::Text("Point Light");
                ImGui::PushID(&l);
                ImGui::DragFloat3("Position", (float*)&l.position, 0.1f);
                ImGui::ColorEdit3("Color", (float*)&l.color);
                ImGui::DragFloat("Intensity", &l.intensity, 0.1f, 0.0001f);
                ImGui::DragFloat("Radius", &l.radius, 0.1f, 0.0001f);
                ImGui::PopID();
            }
            ImGui::TreePop();
        }

        if (ImGui::TreeNode("Atmosphere")) {
            ImGui::Separator();
            AtmosphereData* atmos = reinterpret_cast<AtmosphereData*>(atmosphereDataBuffer->contents());
            bool atmosChanged = false;

            if (!scene->directionalLights.empty()) {
                ImGui::TextColored(ImVec4(0.5f, 0.8f, 1.0f, 1.0f), "Sun synced from first directional light");
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip(
                        "The first directional light in the scene is automatically used as the sun for atmosphere "
                        "rendering."
                    );
                }
            } else {
                ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.5f, 1.0f), "No directional lights - using default sun");
            }
            ImGui::Separator();

            atmosChanged |= ImGui::DragFloat3("Sun Direction", (float*)&atmos->sunDirection, 0.01f, -1.0f, 1.0f);
            if (atmosChanged) {
                atmos->sunDirection = glm::normalize(atmos->sunDirection);
                // Sync back to first directional light if it exists (negate to match convention)
                if (!scene->directionalLights.empty()) {
                    scene->directionalLights[0].direction = -atmos->sunDirection;
                }
            }
            atmosChanged |= ImGui::DragFloat("Sun Intensity", &atmos->sunIntensity, 0.5f, 0.0f, 100.0f);
            if (atmosChanged && !scene->directionalLights.empty()) {
                scene->directionalLights[0].intensity = atmos->sunIntensity;
            }
            atmosChanged |= ImGui::ColorEdit3("Sun Color", (float*)&atmos->sunColor);
            if (atmosChanged && !scene->directionalLights.empty()) {
                scene->directionalLights[0].color = atmos->sunColor;
            }
            atmosChanged |= ImGui::DragFloat("Exposure", &atmos->exposure, 0.01f, 0.01f, 10.0f);
            atmosChanged |= ImGui::ColorEdit3("Ground Color", (float*)&atmos->groundColor);

            if (ImGui::TreeNode("Advanced")) {
                atmosChanged |=
                    ImGui::DragFloat("Planet Radius (m)", &atmos->planetRadius, 1000.0f, 1e3f, 1e8f, "%.0f");
                atmosChanged |=
                    ImGui::DragFloat("Atmosphere Radius (m)", &atmos->atmosphereRadius, 1000.0f, 1e3f, 1e8f, "%.0f");
                atmosChanged |=
                    ImGui::DragFloat("Rayleigh Scale Height", &atmos->rayleighScaleHeight, 100.0f, 100.0f, 50000.0f);
                atmosChanged |= ImGui::DragFloat("Mie Scale Height", &atmos->mieScaleHeight, 100.0f, 100.0f, 10000.0f);
                atmosChanged |=
                    ImGui::DragFloat("Mie Direction (g)", &atmos->miePreferredDirection, 0.01f, -0.999f, 0.999f);

                float rayleighR = atmos->rayleighCoefficients.r * 1e6f;
                float rayleighG = atmos->rayleighCoefficients.g * 1e6f;
                float rayleighB = atmos->rayleighCoefficients.b * 1e6f;
                bool rayleighChanged = false;
                rayleighChanged |= ImGui::DragFloat("Rayleigh R (x1e-6)", &rayleighR, 0.1f, 0.0f, 100.0f);
                rayleighChanged |= ImGui::DragFloat("Rayleigh G (x1e-6)", &rayleighG, 0.1f, 0.0f, 100.0f);
                rayleighChanged |= ImGui::DragFloat("Rayleigh B (x1e-6)", &rayleighB, 0.1f, 0.0f, 100.0f);
                if (rayleighChanged) {
                    atmos->rayleighCoefficients = glm::vec3(rayleighR * 1e-6f, rayleighG * 1e-6f, rayleighB * 1e-6f);
                    atmosChanged = true;
                }

                float mie = atmos->mieCoefficient * 1e6f;
                if (ImGui::DragFloat("Mie Coeff (x1e-6)", &mie, 0.1f, 0.0f, 100.0f)) {
                    atmos->mieCoefficient = mie * 1e-6f;
                    atmosChanged = true;
                }

                if (ImGui::Button("Reset to Earth Defaults")) {
                    atmos->planetRadius = 6371e3f;
                    atmos->atmosphereRadius = 6471e3f;
                    atmos->rayleighScaleHeight = 8500.0f;
                    atmos->mieScaleHeight = 1200.0f;
                    atmos->miePreferredDirection = 0.758f;
                    atmos->rayleighCoefficients = glm::vec3(5.8e-6f, 13.5e-6f, 33.1e-6f);
                    atmos->mieCoefficient = 21e-6f;
                    atmosChanged = true;
                }

                ImGui::TreePop();
            }

            ImGui::Separator();
            ImGui::Text("IBL Status: %s", iblNeedsUpdate ? "Pending Update" : "Up to Date");
            if (ImGui::Button("Refresh IBL")) {
                iblNeedsUpdate = true;
            }
            ImGui::SameLine();
            ImGui::TextDisabled("(?)");
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(
                    "Re-bakes the sky to IBL cubemaps.\nAutomatically triggered when atmosphere parameters change."
                );
            }

            if (atmosChanged) {
                atmosphereDataBuffer->didModifyRange(NS::Range::Make(0, atmosphereDataBuffer->length()));
                iblNeedsUpdate = true;// Trigger IBL update when atmosphere changes
            }
            ImGui::TreePop();
        }

        if (ImGui::TreeNode("Scene Geometry")) {
            ImGui::Separator();
            ImGui::Text("Total vertices: %zu", scene->vertices.size());
            ImGui::Text("Total indices: %zu", scene->indices.size());
            const std::function<void(const std::shared_ptr<Node>&)> showNode = [&](const std::shared_ptr<Node>& node) {
                ImGui::PushID(node.get());
                ImGui::Text("Node #%s", node->name.c_str());
                glm::vec3 pos = node->getLocalPosition();
                glm::vec3 euler = node->getLocalEulerAngles();
                glm::vec3 scale = node->getLocalScale();
                if (ImGui::DragFloat3("Position", &pos.x, 0.1f)) node->setLocalPosition(pos);
                if (ImGui::DragFloat3("Rotation", &euler.x, 1.0f)) node->setLocalEulerAngles(euler);
                if (ImGui::DragFloat3("Scale", &scale.x, 0.1f, 0.0001f)) node->setLocalScale(scale);
                if (node->meshGroup) {
                    for (const auto& mesh : node->meshGroup->meshes) {
                        ImGui::PushID(mesh.get());
                        if (ImGui::TreeNode(fmt::format("Mesh").c_str())) {
                            ImGui::Text("Vertex count: %u", mesh->vertexCount);
                            ImGui::Text("Vertex offset: %u", mesh->vertexOffset);
                            ImGui::Text("Index count: %u", mesh->indexCount);
                            ImGui::Text("Index offset: %u", mesh->indexOffset);
                            ImGui::TreePop();
                        }
                        ImGui::PopID();
                    }
                }
                ImGui::PopID();
                for (const auto& child : node->children) {
                    showNode(child);
                }
            };
            for (const auto& node : scene->nodes) {
                showNode(node);
            }
            ImGui::TreePop();
        }

        if (ImGui::TreeNode("Water Settings")) {
            ImGui::Separator();
            ImGui::Checkbox("Water Enabled", &waterEnabled);

            if (ImGui::TreeNode("Transform")) {
                ImGui::DragFloat3("Position", (float*)&waterTransform.position, 0.1f);
                ImGui::DragFloat3("Scale", (float*)&waterTransform.scale, 0.1f, 0.1f, 10.0f);
                ImGui::TreePop();
            }

            if (ImGui::TreeNode("Colors")) {
                ImGui::ColorEdit4("Surface Color", (float*)&waterSettings.surfaceColor);
                ImGui::ColorEdit4("Refraction Color", (float*)&waterSettings.refractionColor);
                ImGui::TreePop();
            }

            if (ImGui::TreeNode("Wave Parameters")) {
                int waveCount = static_cast<int>(waterSettings.waveCount);
                if (ImGui::SliderInt("Wave Count", &waveCount, 0, 4)) {
                    waterSettings.waveCount = static_cast<Uint32>(waveCount);
                }

                for (Uint32 i = 0; i < waterSettings.waveCount && i < 4; ++i) {
                    ImGui::PushID(i);
                    if (ImGui::TreeNode(fmt::format("Wave {}", i + 1).c_str())) {
                        ImGui::DragFloat3("Direction", (float*)&waterSettings.waves[i].direction, 0.01f, -1.0f, 1.0f);
                        ImGui::DragFloat("Steepness", &waterSettings.waves[i].steepness, 0.01f, 0.0f, 3.0f);
                        ImGui::DragFloat("Wave Length", &waterSettings.waves[i].waveLength, 0.1f, 0.1f, 20.0f);
                        ImGui::DragFloat("Amplitude", &waterSettings.waves[i].amplitude, 0.01f, 0.0f, 5.0f);
                        ImGui::DragFloat("Speed", &waterSettings.waves[i].speed, 0.01f, 0.0f, 5.0f);
                        ImGui::TreePop();
                    }
                    ImGui::PopID();
                }
                ImGui::TreePop();
            }

            if (ImGui::TreeNode("Visual Parameters")) {
                ImGui::DragFloat("Roughness", &waterSettings.roughness, 0.01f, 0.0f, 1.0f);
                ImGui::DragFloat("Reflectance", &waterSettings.reflectance, 0.01f, 0.0f, 1.0f);
                ImGui::DragFloat("Spec Intensity", &waterSettings.specIntensity, 1.0f, 0.0f, 500.0f);
                ImGui::DragFloat("Dampening Factor", &waterSettings.dampeningFactor, 0.1f, 0.1f, 20.0f);
                ImGui::TreePop();
            }

            if (ImGui::TreeNode("Normal Map Scroll")) {
                ImGui::DragFloat2("Scroll Dir 1", (float*)&waterSettings.normalMapScroll, 0.01f, -1.0f, 1.0f);
                ImGui::DragFloat2("Scroll Dir 2", (float*)&waterSettings.normalMapScroll.z, 0.01f, -1.0f, 1.0f);
                ImGui::DragFloat2("Scroll Speed", (float*)&waterSettings.normalMapScrollSpeed, 0.001f, 0.0f, 0.1f);
                ImGui::TreePop();
            }

            if (ImGui::TreeNode("Refraction")) {
                ImGui::DragFloat("Distortion Factor", &waterSettings.refractionDistortionFactor, 0.001f, 0.0f, 0.2f);
                ImGui::DragFloat("Height Factor", &waterSettings.refractionHeightFactor, 0.1f, 0.0f, 10.0f);
                ImGui::DragFloat("Distance Factor", &waterSettings.refractionDistanceFactor, 0.5f, 1.0f, 100.0f);
                ImGui::DragFloat("Depth Softening", &waterSettings.depthSofteningDistance, 0.01f, 0.01f, 5.0f);
                ImGui::TreePop();
            }

            if (ImGui::TreeNode("Foam")) {
                ImGui::DragFloat("Height Start", &waterSettings.foamHeightStart, 0.01f, 0.0f, 2.0f);
                ImGui::DragFloat("Fade Distance", &waterSettings.foamFadeDistance, 0.01f, 0.01f, 2.0f);
                ImGui::DragFloat("Tiling", &waterSettings.foamTiling, 0.1f, 0.1f, 10.0f);
                ImGui::DragFloat("Angle Exponent", &waterSettings.foamAngleExponent, 1.0f, 1.0f, 200.0f);
                ImGui::DragFloat("Brightness", &waterSettings.foamBrightness, 0.1f, 0.1f, 10.0f);
                ImGui::TreePop();
            }

            if (ImGui::TreeNode("SSR Settings")) {
                ImGui::DragFloat("Step Size", &waterSettings.ssrSettings.x, 0.1f, 0.1f, 2.0f);
                ImGui::DragFloat("Max Steps (0=disabled)", &waterSettings.ssrSettings.y, 1.0f, 0.0f, 100.0f);
                ImGui::DragFloat("Refinement Steps", &waterSettings.ssrSettings.z, 1.0f, 1.0f, 50.0f);
                ImGui::DragFloat("Distance Factor", &waterSettings.ssrSettings.w, 1.0f, 1.0f, 100.0f);
                ImGui::TreePop();
            }

            ImGui::TreePop();
        }

        if (ImGui::TreeNode("Light Scattering (God Rays)")) {
            ImGui::Separator();
            ImGui::Checkbox("Enabled", &lightScatteringEnabled);

            if (lightScatteringEnabled) {
                ImGui::Separator();

                AtmosphereData* debugAtmos = reinterpret_cast<AtmosphereData*>(atmosphereDataBuffer->contents());
                glm::vec3 debugSunDir = glm::normalize(debugAtmos->sunDirection);
                glm::vec3 debugCamPos = camera.getEye();
                glm::vec3 debugSunWorldPos = debugCamPos + debugSunDir * 10000.0f;
                glm::mat4 debugViewProj = camera.getProjMatrix() * camera.getViewMatrix();
                glm::vec4 debugSunClip = debugViewProj * glm::vec4(debugSunWorldPos, 1.0f);

                if (debugSunClip.w <= 0.0f) {
                    ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.5f, 1.0f), "Sun behind camera");
                }

                ImGui::Text("Ray Marching");
                int numSamples = static_cast<int>(lightScatteringSettings.numSamples);
                if (ImGui::SliderInt("Samples", &numSamples, 8, 128)) {
                    lightScatteringSettings.numSamples = static_cast<Uint32>(numSamples);
                }
                ImGui::DragFloat("Max Distance", &lightScatteringSettings.maxDistance, 0.01f, 0.1f, 2.0f);

                ImGui::Separator();
                ImGui::Text("Scattering Properties");
                ImGui::DragFloat("Density", &lightScatteringSettings.density, 0.01f, 0.0f, 5.0f);
                ImGui::DragFloat("Weight", &lightScatteringSettings.weight, 0.001f, 0.001f, 0.1f);
                ImGui::DragFloat("Decay", &lightScatteringSettings.decay, 0.001f, 0.9f, 1.0f);
                ImGui::DragFloat("Exposure", &lightScatteringSettings.exposure, 0.01f, 0.0f, 2.0f);

                ImGui::Separator();
                ImGui::Text("Light Properties");
                ImGui::DragFloat("Sun Intensity", &lightScatteringSettings.sunIntensity, 0.1f, 0.0f, 10.0f);
                ImGui::DragFloat("Mie G (Phase)", &lightScatteringSettings.mieG, 0.01f, -0.99f, 0.99f);
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip(
                        "Mie scattering direction:\n< 0: backscatter\n= 0: isotropic\n> 0: forward scatter (sun glare)"
                    );
                }

                ImGui::Separator();
                ImGui::Text("Advanced");
                ImGui::DragFloat(
                    "Depth Threshold", &lightScatteringSettings.depthThreshold, 0.0001f, 0.99f, 1.0f, "%.4f"
                );
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip(
                        "Depth value above which pixels are considered 'sky'.\nHigher = only sky contributes to rays."
                    );
                }
                ImGui::DragFloat("Temporal Jitter", &lightScatteringSettings.jitter, 0.01f, 0.0f, 1.0f);
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Jitter amount for temporal anti-aliasing.\nReduces banding artifacts.");
                }

                if (ImGui::Button("Reset to Defaults")) {
                    lightScatteringSettings.density = 1.0f;
                    lightScatteringSettings.weight = 0.01f;
                    lightScatteringSettings.decay = 0.97f;
                    lightScatteringSettings.exposure = 0.3f;
                    lightScatteringSettings.numSamples = 64;
                    lightScatteringSettings.maxDistance = 1.0f;
                    lightScatteringSettings.sunIntensity = 1.0f;
                    lightScatteringSettings.mieG = 0.76f;
                    lightScatteringSettings.depthThreshold = 0.9999f;
                    lightScatteringSettings.jitter = 0.5f;
                }
            }
            ImGui::TreePop();
        }

        if (ImGui::TreeNode("Height Fog")) {
            ImGui::Separator();
            ImGui::Checkbox("Enabled", &volumetricFogEnabled);

            if (volumetricFogEnabled) {
                ImGui::Separator();
                ImGui::Text("Fog Parameters");
                ImGui::DragFloat("Density", &volumetricFogSettings.fogDensity, 0.001f, 0.0f, 0.5f);
                ImGui::DragFloat("Height Falloff", &volumetricFogSettings.fogHeightFalloff, 0.01f, 0.001f, 1.0f);
                ImGui::DragFloat("Base Height", &volumetricFogSettings.fogBaseHeight, 1.0f, -100.0f, 100.0f);
                ImGui::DragFloat("Max Height", &volumetricFogSettings.fogMaxHeight, 10.0f, 0.0f, 500.0f);

                ImGui::Separator();
                ImGui::Text("Scattering");
                ImGui::DragFloat("Anisotropy", &volumetricFogSettings.anisotropy, 0.01f, -0.99f, 0.99f);
                ImGui::DragFloat("Ambient Intensity", &volumetricFogSettings.ambientIntensity, 0.01f, 0.0f, 2.0f);

                if (ImGui::Button("Reset to Defaults")) {
                    volumetricFogSettings.fogDensity = 0.02f;
                    volumetricFogSettings.fogHeightFalloff = 0.1f;
                    volumetricFogSettings.fogBaseHeight = 0.0f;
                    volumetricFogSettings.fogMaxHeight = 100.0f;
                    volumetricFogSettings.anisotropy = 0.6f;
                    volumetricFogSettings.ambientIntensity = 0.3f;
                }
            }
            ImGui::TreePop();
        }

        if (ImGui::TreeNode("Volumetric Clouds")) {
            ImGui::Separator();
            ImGui::Checkbox("Enabled", &volumetricCloudsEnabled);

            if (volumetricCloudsEnabled) {
                ImGui::Separator();
                ImGui::Text("Cloud Layer");
                ImGui::DragFloat("Bottom (m)", &volumetricCloudSettings.cloudLayerBottom, 100.0f, 0.0f, 10000.0f);
                ImGui::DragFloat("Top (m)", &volumetricCloudSettings.cloudLayerTop, 100.0f, 0.0f, 15000.0f);
                ImGui::DragFloat("Coverage", &volumetricCloudSettings.cloudCoverage, 0.01f, 0.0f, 1.0f);
                ImGui::DragFloat("Density", &volumetricCloudSettings.cloudDensity, 0.01f, 0.0f, 1.0f);
                ImGui::DragFloat("Type (Stratus-Cumulus)", &volumetricCloudSettings.cloudType, 0.01f, 0.0f, 1.0f);

                ImGui::Separator();
                ImGui::Text("Lighting");
                ImGui::DragFloat("Ambient", &volumetricCloudSettings.ambientIntensity, 0.01f, 0.0f, 1.0f);
                ImGui::DragFloat("Silver Lining", &volumetricCloudSettings.silverLiningIntensity, 0.01f, 0.0f, 2.0f);
            }
            ImGui::TreePop();
        }

        if (ImGui::TreeNode("Sun Flare (Lens Flare)")) {
            ImGui::Separator();
            ImGui::Checkbox("Enabled", &sunFlareEnabled);

            ImGui::DragFloat("Sun Intensity", &sunFlareSettings.sunIntensity, 0.1f, 0.0f, 100.0f);
            ImGui::ColorEdit3("Sun Color", &sunFlareSettings.sunColor[0]);
            ImGui::DragFloat("Fade Edge", &sunFlareSettings.fadeEdge, 0.01f, 0.0f, 1.0f);

            ImGui::Separator();
            ImGui::Text("Glow");
            ImGui::DragFloat("Glow Intensity", &sunFlareSettings.glowIntensity, 0.01f, 0.0f, 2.0f);
            ImGui::DragFloat("Glow Falloff", &sunFlareSettings.glowFalloff, 0.1f, 0.1f, 20.0f);
            ImGui::DragFloat("Glow Size", &sunFlareSettings.glowSize, 0.01f, 0.0f, 2.0f);

            ImGui::Separator();
            ImGui::Text("Halo");
            ImGui::DragFloat("Halo Intensity", &sunFlareSettings.haloIntensity, 0.01f, 0.0f, 1.0f);
            ImGui::DragFloat("Halo Radius", &sunFlareSettings.haloRadius, 0.01f, 0.0f, 1.0f);
            ImGui::DragFloat("Halo Width", &sunFlareSettings.haloWidth, 0.01f, 0.0f, 0.5f);
            ImGui::DragFloat("Halo Falloff", &sunFlareSettings.haloFalloff, 0.01f, 0.0f, 1.0f);

            ImGui::Separator();
            ImGui::Text("Ghosts");
            int count = static_cast<int>(sunFlareSettings.ghostCount);
            if (ImGui::SliderInt("Ghost Count", &count, 0, 10)) {
                sunFlareSettings.ghostCount = static_cast<Uint32>(count);
            }
            ImGui::DragFloat("Ghost Spacing", &sunFlareSettings.ghostSpacing, 0.01f, -1.0f, 1.0f);
            ImGui::DragFloat("Ghost Intensity", &sunFlareSettings.ghostIntensity, 0.01f, 0.0f, 1.0f);
            ImGui::DragFloat("Ghost Size", &sunFlareSettings.ghostSize, 0.01f, 0.0f, 0.5f);
            ImGui::DragFloat("Ghost Chromatic", &sunFlareSettings.ghostChromaticOffset, 0.001f, 0.0f, 0.05f);
            ImGui::DragFloat("Ghost Falloff", &sunFlareSettings.ghostFalloff, 0.1f, 0.1f, 10.0f);

            ImGui::Separator();
            ImGui::Text("Streak");
            ImGui::DragFloat("Streak Intensity", &sunFlareSettings.streakIntensity, 0.01f, 0.0f, 1.0f);
            ImGui::DragFloat("Streak Length", &sunFlareSettings.streakLength, 0.01f, 0.0f, 2.0f);
            ImGui::DragFloat("Streak Falloff", &sunFlareSettings.streakFalloff, 0.1f, 0.1f, 100.0f);

            ImGui::Separator();
            ImGui::Text("Starburst");
            ImGui::DragFloat("Starburst Intensity", &sunFlareSettings.starburstIntensity, 0.01f, 0.0f, 1.0f);
            ImGui::DragFloat("Starburst Size", &sunFlareSettings.starburstSize, 0.01f, 0.0f, 2.0f);
            int points = static_cast<int>(sunFlareSettings.starburstPoints);
            if (ImGui::SliderInt("Starburst Points", &points, 0, 16)) {
                sunFlareSettings.starburstPoints = static_cast<Uint32>(points);
            }
            ImGui::DragFloat("Starburst Rotation", &sunFlareSettings.starburstRotation, 0.01f, -3.14f, 3.14f);

            ImGui::Separator();
            ImGui::Text("Dirt");
            ImGui::DragFloat("Dirt Intensity", &sunFlareSettings.dirtIntensity, 0.01f, 0.0f, 1.0f);
            ImGui::DragFloat("Dirt Scale", &sunFlareSettings.dirtScale, 0.1f, 0.1f, 20.0f);

            ImGui::TreePop();
        }
    }

    // ==========================================================================
    // Execute all render passes
    // ==========================================================================
    graph.execute();

    // ==========================================================================
    // Present and cleanup
    // ==========================================================================
    cmd->presentDrawable(surface);
    cmd->commit();

    // Note: Don't call surface->release() here!
    // nextDrawable() returns an autoreleased object that will be managed by the system AutoreleasePool.
    // presentDrawable() retains the drawable until presentation completes.
    // The system AutoreleasePool (managed by the main run loop) will automatically release it.

    currentFrameInFlight = (currentFrameInFlight + 1) % MAX_FRAMES_IN_FLIGHT;
    frameNumber++;
}


NS::SharedPtr<MTL::RenderPipelineState>
    Renderer_Metal::createPipeline(const std::string& filename, bool isHDR, bool isColorOnly, Uint32 sampleCount) {
    auto shaderSrc = readFile(filename);

    auto code = NS::String::string(shaderSrc.data(), NS::StringEncoding::UTF8StringEncoding);
    NS::Error* error = nullptr;
    MTL::CompileOptions* options = nullptr;
    MTL::Library* library = device->newLibrary(code, options, &error);
    if (!library) {
        throw std::runtime_error(
            fmt::format("Could not compile shader! Error: {}\n", error->localizedDescription()->utf8String())
        );
    }
    // fmt::print("Shader compiled successfully. Shader: {}\n", code->cString(NS::StringEncoding::UTF8StringEncoding));

    auto vertexFuncName = NS::String::string("vertexMain", NS::StringEncoding::UTF8StringEncoding);
    auto vertexMain = library->newFunction(vertexFuncName);

    auto fragmentFuncName = NS::String::string("fragmentMain", NS::StringEncoding::UTF8StringEncoding);
    auto fragmentMain = library->newFunction(fragmentFuncName);

    auto pipelineDesc = MTL::RenderPipelineDescriptor::alloc()->init();
    pipelineDesc->setVertexFunction(vertexMain);
    pipelineDesc->setFragmentFunction(fragmentMain);

    // auto vertexDesc = MTL::VertexDescriptor::alloc()->init();

    // auto layout = vertexDesc->layouts()->object(0);
    // layout->setStride(sizeof(VertexData));
    // layout->setStepFunction(MTL::VertexStepFunctionPerVertex);
    // layout->setStepRate(1);

    // auto attributes = vertexDesc->attributes();

    // auto posAttr = attributes->object(0);
    // posAttr->setFormat(MTL::VertexFormatFloat3);
    // posAttr->setOffset(offsetof(VertexData, position));
    // posAttr->setBufferIndex(2);

    // auto uvAttr = attributes->object(1);
    // uvAttr->setFormat(MTL::VertexFormatFloat2);
    // uvAttr->setOffset(offsetof(VertexData, uv));
    // uvAttr->setBufferIndex(2);

    // auto normalAttr = attributes->object(2);
    // normalAttr->setFormat(MTL::VertexFormatFloat3);
    // normalAttr->setOffset(offsetof(VertexData, normal));
    // normalAttr->setBufferIndex(2);

    // auto tangentAttr = attributes->object(3);
    // tangentAttr->setFormat(MTL::VertexFormatFloat4);
    // tangentAttr->setOffset(offsetof(VertexData, tangent));
    // tangentAttr->setBufferIndex(2);

    // pipelineDesc->setVertexDescriptor(vertexDesc);

    auto colorAttachment = pipelineDesc->colorAttachments()->object(0);
    if (isHDR) {
        colorAttachment->setPixelFormat(MTL::PixelFormat::PixelFormatRGBA16Float);// HDR format
    } else {
        colorAttachment->setPixelFormat(swapchain->pixelFormat());
    }
    // TODO: optinal blending for particles
    // colorAttachment->setBlendingEnabled(true);
    // colorAttachment->setAlphaBlendOperation(MTL::BlendOperation::BlendOperationAdd);
    // colorAttachment->setRgbBlendOperation(MTL::BlendOperation::BlendOperationAdd);
    // colorAttachment->setSourceRGBBlendFactor(MTL::BlendFactor::BlendFactorSourceAlpha);
    // colorAttachment->setSourceAlphaBlendFactor(MTL::BlendFactor::BlendFactorSourceAlpha);
    // colorAttachment->setDestinationRGBBlendFactor(MTL::BlendFactor::BlendFactorOneMinusSourceAlpha);
    // colorAttachment->setDestinationAlphaBlendFactor(MTL::BlendFactor::BlendFactorOneMinusSourceAlpha);
    if (!isColorOnly) {
        pipelineDesc->setDepthAttachmentPixelFormat(MTL::PixelFormatDepth32Float);
    } else {
        pipelineDesc->setDepthAttachmentPixelFormat(MTL::PixelFormatInvalid);
    }
    pipelineDesc->setSampleCount(static_cast<NS::UInteger>(sampleCount));

    auto pipeline = NS::TransferPtr(device->newRenderPipelineState(pipelineDesc, &error));
    if (!pipeline) {
        std::string errorMsg = error ? error->localizedDescription()->utf8String() : "Unknown error";
        throw std::runtime_error(fmt::format("Could not create pipeline! Error: {}\nShader: {}\n", errorMsg, filename));
    }

    code->release();
    library->release();
    vertexMain->release();
    fragmentMain->release();
    // vertexDesc->release();
    pipelineDesc->release();

    return pipeline;
}

NS::SharedPtr<MTL::ComputePipelineState> Renderer_Metal::createComputePipeline(const std::string& filename) {
    auto shaderSrc = readFile(filename);

    auto code = NS::String::string(shaderSrc.data(), NS::StringEncoding::UTF8StringEncoding);
    NS::Error* error = nullptr;
    MTL::CompileOptions* options = nullptr;
    MTL::Library* library = device->newLibrary(code, options, &error);
    if (!library) {
        throw std::runtime_error(
            fmt::format("Could not compile shader! Error: {}\n", error->localizedDescription()->utf8String())
        );
    }
    // fmt::print("Shader compiled successfully. Shader: {}\n", code->cString(NS::StringEncoding::UTF8StringEncoding));

    auto computeFuncName = NS::String::string("computeMain", NS::StringEncoding::UTF8StringEncoding);
    auto computeMain = library->newFunction(computeFuncName);

    auto pipeline = NS::TransferPtr(device->newComputePipelineState(computeMain, &error));

    code->release();
    library->release();
    computeMain->release();

    return pipeline;
}

TextureHandle Renderer_Metal::createTexture(const std::shared_ptr<Image>& img) {
    if (img) {
        MTL::PixelFormat pixelFormat = MTL::PixelFormat::PixelFormatRGBA8Unorm;
        switch (img->channelCount) {
        case 1:
            pixelFormat = MTL::PixelFormat::PixelFormatR8Unorm;
            break;
        case 2:
            pixelFormat = MTL::PixelFormat::PixelFormatRG8Unorm;
            break;
        case 3:
        case 4:
            pixelFormat = MTL::PixelFormat::PixelFormatRGBA8Unorm;
            break;
        default:
            throw std::runtime_error(fmt::format(
                "Unknown texture format at {} (channelCount={}, width={}, height={}, byteArraySize={})\n",
                img->uri,
                img->channelCount,
                img->width,
                img->height,
                img->byteArray.size()
            ));
            break;
        }
        int numLevels = calculateMipmapLevelCount(img->width, img->height);

        auto textureDesc = NS::TransferPtr(MTL::TextureDescriptor::alloc()->init());
        textureDesc->setPixelFormat(pixelFormat);
        textureDesc->setTextureType(MTL::TextureType::TextureType2D);
        textureDesc->setWidth(NS::UInteger(img->width));
        textureDesc->setHeight(NS::UInteger(img->height));
        textureDesc->setMipmapLevelCount(numLevels);
        textureDesc->setSampleCount(1);
        textureDesc->setStorageMode(MTL::StorageMode::StorageModeManaged);
        textureDesc->setUsage(MTL::ResourceUsageSample | MTL::ResourceUsageRead);

        auto texture = NS::TransferPtr(device->newTexture(textureDesc.get()));
        if (img->channelCount == 3) {
            // Convert RGB to RGBA by adding alpha channel
            std::vector<Uint8> rgbaData;
            rgbaData.reserve(img->width * img->height * 4);
            for (size_t i = 0; i < img->byteArray.size(); i += 3) {
                rgbaData.push_back(img->byteArray[i]);// R
                rgbaData.push_back(img->byteArray[i + 1]);// G
                rgbaData.push_back(img->byteArray[i + 2]);// B
                rgbaData.push_back(255);// A (opaque)
            }
            texture->replaceRegion(
                MTL::Region(0, 0, 0, img->width, img->height, 1), 0, rgbaData.data(), img->width * 4
            );
        } else {
            size_t bytesPerPixel = img->channelCount;
            texture->replaceRegion(
                MTL::Region(0, 0, 0, img->width, img->height, 1), 0, img->byteArray.data(), img->width * bytesPerPixel
            );
        }

        if (numLevels > 1) {
            auto cmdBlit = NS::TransferPtr(queue->commandBuffer());
            auto enc = NS::TransferPtr(cmdBlit->blitCommandEncoder());
            enc->generateMipmaps(texture.get());
            enc->endEncoding();
            cmdBlit->commit();
        }

        textures[nextTextureID] = texture;

        return TextureHandle{ nextTextureID++ };
    } else {
        throw std::runtime_error(fmt::format("Failed to create texture at {}!\n", img->uri));
    }
}

// ===== Font Rendering Implementation =====

FontHandle Renderer_Metal::loadFont(const std::string& path, float baseSize) {
    // Load font using FontManager
    FontHandle fontHandle = m_fontManager.loadFont(path, baseSize);
    if (!fontHandle.isValid()) {
        return fontHandle;
    }

    // Get atlas data and create Metal texture
    const FontManager::AtlasData* atlasData = m_fontManager.getAtlasData(fontHandle);
    if (!atlasData) {
        m_fontManager.unloadFont(fontHandle);
        return FontHandle{};
    }

    // Create texture from atlas data
    auto textureDesc = NS::TransferPtr(MTL::TextureDescriptor::alloc()->init());
    textureDesc->setPixelFormat(MTL::PixelFormat::PixelFormatRGBA8Unorm);
    textureDesc->setTextureType(MTL::TextureType::TextureType2D);
    textureDesc->setWidth(NS::UInteger(atlasData->width));
    textureDesc->setHeight(NS::UInteger(atlasData->height));
    textureDesc->setMipmapLevelCount(1);
    textureDesc->setSampleCount(1);
    textureDesc->setStorageMode(MTL::StorageMode::StorageModeManaged);
    textureDesc->setUsage(MTL::ResourceUsageSample | MTL::ResourceUsageRead);

    auto texture = NS::TransferPtr(device->newTexture(textureDesc.get()));
    texture->replaceRegion(
        MTL::Region(0, 0, 0, atlasData->width, atlasData->height, 1),
        0,
        atlasData->rgbaData.data(),
        atlasData->width * 4
    );

    // Store texture and create handle
    textures[nextTextureID] = texture;
    TextureHandle texHandle{nextTextureID++};

    // Associate texture handle with font
    m_fontManager.setFontTextureHandle(fontHandle, texHandle);

    return fontHandle;
}

void Renderer_Metal::unloadFont(FontHandle handle) {
    if (!handle.isValid()) return;

    // Get texture handle before unloading
    TextureHandle texHandle = m_fontManager.getFontTexture(handle);
    if (texHandle.rid != UINT32_MAX) {
        textures.erase(texHandle.rid);
    }

    m_fontManager.unloadFont(handle);
}

void Renderer_Metal::drawText2D(
    FontHandle fontHandle,
    const std::string& text,
    const glm::vec2& position,
    float scale,
    const glm::vec4& color
) {
    Font* font = m_fontManager.getFont(fontHandle);
    if (!font || font->textureHandle.rid == UINT32_MAX) return;

    float cursorX = position.x;
    float cursorY = position.y;

    for (char c : text) {
        const Glyph* glyph = m_fontManager.getGlyph(fontHandle, static_cast<int>(c));
        if (!glyph) continue;

        float drawX = cursorX + glyph->xOffset * scale;
        float drawY = cursorY + glyph->yOffset * scale + font->ascent * scale;
        float drawW = glyph->width * scale;
        float drawH = glyph->height * scale;

        if (drawW > 0 && drawH > 0) {
            // Adjust for centered quad rendering (batchQuadPositions uses -0.5 to 0.5)
            float finalX = drawX + drawW * 0.5f;
            float finalY = drawY + drawH * 0.5f;

            // Create UV coordinates for this glyph
            glm::vec2 uvs[4] = {
                {glyph->u0, glyph->v0}, // top-left
                {glyph->u1, glyph->v0}, // top-right
                {glyph->u1, glyph->v1}, // bottom-right
                {glyph->u0, glyph->v1}  // bottom-left
            };

            glm::mat4 transform = glm::translate(glm::mat4(1.0f), glm::vec3(finalX, finalY, 0.0f));
            transform = glm::scale(transform, glm::vec3(drawW, drawH, 1.0f));
            drawQuad2D(transform, font->textureHandle, uvs, color);
        }

        cursorX += glyph->advance * scale;
    }
}

void Renderer_Metal::drawText3D(
    FontHandle fontHandle,
    const std::string& text,
    const glm::vec3& worldPosition,
    float scale,
    const glm::vec4& color
) {
    Font* font = m_fontManager.getFont(fontHandle);
    if (!font || font->textureHandle.rid == UINT32_MAX) return;

    // For 3D text, we draw at the world position
    // The text will be rendered as billboards facing the camera
    float cursorX = 0.0f;

    for (char c : text) {
        const Glyph* glyph = m_fontManager.getGlyph(fontHandle, static_cast<int>(c));
        if (!glyph) continue;

        float drawX = cursorX + glyph->xOffset * scale;
        float drawY = glyph->yOffset * scale + font->ascent * scale;
        float drawW = glyph->width * scale;
        float drawH = glyph->height * scale;

        if (drawW > 0 && drawH > 0) {
            // Adjust for centered quad rendering (batchQuadPositions uses -0.5 to 0.5)
            float finalX = drawX + drawW * 0.5f;
            float finalY = drawY + drawH * 0.5f;

            // Create UV coordinates for this glyph
            glm::vec2 uvs[4] = {
                {glyph->u0, glyph->v0}, // top-left
                {glyph->u1, glyph->v0}, // top-right
                {glyph->u1, glyph->v1}, // bottom-right
                {glyph->u0, glyph->v1}  // bottom-left
            };

            // Create transform in world space
            glm::mat4 transform = glm::translate(glm::mat4(1.0f), worldPosition);
            transform = glm::translate(transform, glm::vec3(finalX, finalY, 0.0f));
            transform = glm::scale(transform, glm::vec3(drawW, drawH, 1.0f));
            drawQuad3D(transform, font->textureHandle, uvs, color);
        }

        cursorX += glyph->advance * scale;
    }
}

glm::vec2 Renderer_Metal::measureText(FontHandle fontHandle, const std::string& text, float scale) {
    return m_fontManager.measureText(fontHandle, text, scale);
}

float Renderer_Metal::getFontLineHeight(FontHandle fontHandle, float scale) {
    Font* font = m_fontManager.getFont(fontHandle);
    return font ? font->lineHeight * scale : 0.0f;
}

BufferHandle Renderer_Metal::createVertexBuffer(const std::vector<VertexData>& vertices) {
    auto stagingBuffer =
        NS::TransferPtr(device->newBuffer(vertices.size() * sizeof(VertexData), MTL::ResourceStorageModeShared));
    memcpy(stagingBuffer->contents(), vertices.data(), vertices.size() * sizeof(VertexData));

    auto buffer =
        NS::TransferPtr(device->newBuffer(vertices.size() * sizeof(VertexData), MTL::ResourceStorageModePrivate));

    auto cmd = queue->commandBuffer();
    auto blitEncoder = cmd->blitCommandEncoder();
    blitEncoder->copyFromBuffer(stagingBuffer.get(), 0, buffer.get(), 0, vertices.size() * sizeof(VertexData));
    blitEncoder->endEncoding();
    cmd->commit();

    buffers[nextBufferID] = buffer;

    return BufferHandle{ nextBufferID++ };
}

BufferHandle Renderer_Metal::createIndexBuffer(const std::vector<Uint32>& indices) {
    auto stagingBuffer =
        NS::TransferPtr(device->newBuffer(indices.size() * sizeof(Uint32), MTL::ResourceStorageModeShared));
    memcpy(stagingBuffer->contents(), indices.data(), indices.size() * sizeof(Uint32));

    auto buffer = NS::TransferPtr(device->newBuffer(indices.size() * sizeof(Uint32), MTL::ResourceStorageModePrivate));

    auto cmd = queue->commandBuffer();
    auto blitEncoder = cmd->blitCommandEncoder();
    blitEncoder->copyFromBuffer(stagingBuffer.get(), 0, buffer.get(), 0, indices.size() * sizeof(Uint32));
    blitEncoder->endEncoding();
    cmd->commit();

    buffers[nextBufferID] = buffer;

    return BufferHandle{ nextBufferID++ };
}

NS::SharedPtr<MTL::Buffer> Renderer_Metal::getBuffer(BufferHandle handle) const {
    return buffers.at(handle.rid);
}

NS::SharedPtr<MTL::Texture> Renderer_Metal::getTexture(TextureHandle handle) const {
    return textures.at(handle.rid);
}

NS::SharedPtr<MTL::RenderPipelineState> Renderer_Metal::getPipeline(PipelineHandle handle) const {
    return pipelines.at(handle.rid);
}

// ===== 2D/3D Batch Rendering Implementation =====

void Renderer_Metal::beginBatch2D() {
    if (batch2DActive) return;
    batch2DVertices.clear();
    batch2DIndices.clear();
    batch2DTextureSlots[0] = batch2DWhiteTextureHandle;
    batch2DTextureSlotIndex = 1;
    batch2DActive = true;
}

void Renderer_Metal::endBatch2D() {
    batch2DActive = false;
}

void Renderer_Metal::beginBatch3D() {
    if (batch3DActive) return;
    batch3DVertices.clear();
    batch3DIndices.clear();
    batch3DTextureSlots[0] = batch2DWhiteTextureHandle;
    batch3DTextureSlotIndex = 1;
    batch3DActive = true;
}

void Renderer_Metal::endBatch3D() {
    batch3DActive = false;
}

void Renderer_Metal::flush2D() {
    // Will be rendered by CanvasPass
    endBatch2D();
}

void Renderer_Metal::flush3D() {
    // Will be rendered by WorldCanvasPass
    endBatch3D();
}

// Helper to find or add a texture slot
static float findOrAddTextureSlot(
    std::array<TextureHandle, 16>& slots, Uint32& slotIndex, TextureHandle texture, TextureHandle whiteTexture
) {
    if (texture.rid == UINT32_MAX || texture.rid == whiteTexture.rid) {
        return 0.0f;
    }

    for (Uint32 i = 1; i < slotIndex; i++) {
        if (slots[i].rid == texture.rid) {
            return static_cast<float>(i);
        }
    }

    if (slotIndex >= 16) {
        return 0.0f;// Fallback to white texture if slots full
    }

    float texIndex = static_cast<float>(slotIndex);
    slots[slotIndex] = texture;
    slotIndex++;
    return texIndex;
}

void Renderer_Metal::drawQuad2D(const glm::vec2& position, const glm::vec2& size, const glm::vec4& color) {
    drawQuad2D(glm::vec3(position, 0.0f), size, color);
}

void Renderer_Metal::drawQuad2D(const glm::vec3& position, const glm::vec2& size, const glm::vec4& color) {
    glm::mat4 transform =
        glm::translate(glm::mat4(1.0f), position) * glm::scale(glm::mat4(1.0f), glm::vec3(size, 1.0f));
    drawQuad2D(transform, color);
}

void Renderer_Metal::drawQuad2D(
    const glm::vec2& position, const glm::vec2& size, TextureHandle texture, const glm::vec4& tintColor
) {
    glm::mat4 transform =
        glm::translate(glm::mat4(1.0f), glm::vec3(position, 0.0f)) * glm::scale(glm::mat4(1.0f), glm::vec3(size, 1.0f));
    drawQuad2D(transform, texture, batchQuadTexCoords, tintColor);
}

void Renderer_Metal::drawQuad2D(const glm::mat4& transform, const glm::vec4& color, int entityID) {
    drawQuad2D(transform, batch2DWhiteTextureHandle, batchQuadTexCoords, color, entityID);
}

void Renderer_Metal::drawQuad2D(
    const glm::mat4& transform,
    TextureHandle texture,
    const glm::vec2* texCoords,
    const glm::vec4& tintColor,
    int entityID
) {
    beginBatch2D();// Auto-start batch
    if (batch2DIndices.size() >= BatchMaxIndices) {
        return;// Batch full
    }

    float textureIndex =
        findOrAddTextureSlot(batch2DTextureSlots, batch2DTextureSlotIndex, texture, batch2DWhiteTextureHandle);
    Uint32 vertexOffset = static_cast<Uint32>(batch2DVertices.size());

    // Add 4 vertices
    for (int i = 0; i < 4; i++) {
        Batch2DVertex vertex;
        vertex.position = transform * batchQuadPositions[i];
        vertex.color = tintColor;
        vertex.uv = texCoords[i];
        vertex.texIndex = textureIndex;
        vertex.entityID = static_cast<float>(entityID);
        batch2DVertices.push_back(vertex);
    }

    // Add 6 indices (2 triangles)
    batch2DIndices.push_back(vertexOffset + 0);
    batch2DIndices.push_back(vertexOffset + 1);
    batch2DIndices.push_back(vertexOffset + 2);
    batch2DIndices.push_back(vertexOffset + 2);
    batch2DIndices.push_back(vertexOffset + 3);
    batch2DIndices.push_back(vertexOffset + 0);

    batch2DStats.quadCount++;
}

void Renderer_Metal::drawRotatedQuad2D(
    const glm::vec2& position, const glm::vec2& size, float rotation, const glm::vec4& color
) {
    glm::mat4 transform = glm::translate(glm::mat4(1.0f), glm::vec3(position, 0.0f))
                          * glm::rotate(glm::mat4(1.0f), rotation, glm::vec3(0.0f, 0.0f, 1.0f))
                          * glm::scale(glm::mat4(1.0f), glm::vec3(size, 1.0f));
    drawQuad2D(transform, color);
}

void Renderer_Metal::drawRotatedQuad2D(
    const glm::vec2& position, const glm::vec2& size, float rotation, TextureHandle texture, const glm::vec4& tintColor
) {
    glm::mat4 transform = glm::translate(glm::mat4(1.0f), glm::vec3(position, 0.0f))
                          * glm::rotate(glm::mat4(1.0f), rotation, glm::vec3(0.0f, 0.0f, 1.0f))
                          * glm::scale(glm::mat4(1.0f), glm::vec3(size, 1.0f));
    drawQuad2D(transform, texture, batchQuadTexCoords, tintColor);
}

void Renderer_Metal::drawLine2D(const glm::vec2& p0, const glm::vec2& p1, const glm::vec4& color, float thickness) {
    beginBatch2D();// Auto-start batch

    glm::vec2 direction = p1 - p0;
    float length = glm::length(direction);
    if (length < 0.0001f) return;

    glm::vec2 normalized = direction / length;
    glm::vec2 perpendicular(-normalized.y, normalized.x);
    float halfThickness = thickness * 0.5f;

    // Four corners of the line quad
    glm::vec3 v0 = glm::vec3(p0 - perpendicular * halfThickness, 0.0f);
    glm::vec3 v1 = glm::vec3(p1 - perpendicular * halfThickness, 0.0f);
    glm::vec3 v2 = glm::vec3(p1 + perpendicular * halfThickness, 0.0f);
    glm::vec3 v3 = glm::vec3(p0 + perpendicular * halfThickness, 0.0f);

    if (batch2DIndices.size() >= BatchMaxIndices) return;

    glm::vec2 defaultUV(0.5f, 0.5f);
    Uint32 vertexOffset = static_cast<Uint32>(batch2DVertices.size());

    Batch2DVertex vertex;
    vertex.color = color;
    vertex.uv = defaultUV;
    vertex.texIndex = 0.0f;
    vertex.entityID = -1.0f;

    vertex.position = v0;
    batch2DVertices.push_back(vertex);
    vertex.position = v1;
    batch2DVertices.push_back(vertex);
    vertex.position = v2;
    batch2DVertices.push_back(vertex);
    vertex.position = v3;
    batch2DVertices.push_back(vertex);

    batch2DIndices.push_back(vertexOffset + 0);
    batch2DIndices.push_back(vertexOffset + 1);
    batch2DIndices.push_back(vertexOffset + 2);
    batch2DIndices.push_back(vertexOffset + 2);
    batch2DIndices.push_back(vertexOffset + 3);
    batch2DIndices.push_back(vertexOffset + 0);

    batch2DStats.lineCount++;
}

// ===== 3D Batch Drawing (world space with depth) =====

void Renderer_Metal::drawQuad3D(const glm::vec3& position, const glm::vec2& size, const glm::vec4& color) {
    glm::mat4 transform =
        glm::translate(glm::mat4(1.0f), position) * glm::scale(glm::mat4(1.0f), glm::vec3(size, 1.0f));
    drawQuad3D(transform, color);
}

void Renderer_Metal::drawQuad3D(
    const glm::vec3& position, const glm::vec2& size, TextureHandle texture, const glm::vec4& tintColor
) {
    glm::mat4 transform =
        glm::translate(glm::mat4(1.0f), position) * glm::scale(glm::mat4(1.0f), glm::vec3(size, 1.0f));
    drawQuad3D(transform, texture, batchQuadTexCoords, tintColor);
}

void Renderer_Metal::drawQuad3D(const glm::mat4& transform, const glm::vec4& color, int entityID) {
    drawQuad3D(transform, batch2DWhiteTextureHandle, batchQuadTexCoords, color, entityID);
}

void Renderer_Metal::drawQuad3D(
    const glm::mat4& transform,
    TextureHandle texture,
    const glm::vec2* texCoords,
    const glm::vec4& tintColor,
    int entityID
) {
    beginBatch3D();// Auto-start batch
    if (batch3DIndices.size() >= BatchMaxIndices) return;

    float textureIndex =
        findOrAddTextureSlot(batch3DTextureSlots, batch3DTextureSlotIndex, texture, batch2DWhiteTextureHandle);
    Uint32 vertexOffset = static_cast<Uint32>(batch3DVertices.size());

    for (int i = 0; i < 4; i++) {
        Batch2DVertex vertex;
        vertex.position = transform * batchQuadPositions[i];
        vertex.color = tintColor;
        vertex.uv = texCoords[i];
        vertex.texIndex = textureIndex;
        vertex.entityID = static_cast<float>(entityID);
        batch3DVertices.push_back(vertex);
    }

    batch3DIndices.push_back(vertexOffset + 0);
    batch3DIndices.push_back(vertexOffset + 1);
    batch3DIndices.push_back(vertexOffset + 2);
    batch3DIndices.push_back(vertexOffset + 2);
    batch3DIndices.push_back(vertexOffset + 3);
    batch3DIndices.push_back(vertexOffset + 0);

    batch3DStats.quadCount++;
}

void Renderer_Metal::drawLine3D(const glm::vec3& p0, const glm::vec3& p1, const glm::vec4& color, float thickness) {
    beginBatch3D();// Auto-start batch

    glm::vec3 direction = p1 - p0;
    float length = glm::length(direction);
    if (length < 0.0001f) return;

    glm::vec3 normalized = direction / length;
    // For 3D lines, we need a perpendicular that works in 3D space
    glm::vec3 up = (std::abs(normalized.y) < 0.999f) ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0);
    glm::vec3 perpendicular = glm::normalize(glm::cross(normalized, up));
    float halfThickness = thickness * 0.5f;

    glm::vec3 v0 = p0 - perpendicular * halfThickness;
    glm::vec3 v1 = p1 - perpendicular * halfThickness;
    glm::vec3 v2 = p1 + perpendicular * halfThickness;
    glm::vec3 v3 = p0 + perpendicular * halfThickness;

    if (batch3DIndices.size() >= BatchMaxIndices) return;

    glm::vec2 defaultUV(0.5f, 0.5f);
    Uint32 vertexOffset = static_cast<Uint32>(batch3DVertices.size());

    Batch2DVertex vertex;
    vertex.color = color;
    vertex.uv = defaultUV;
    vertex.texIndex = 0.0f;
    vertex.entityID = -1.0f;

    vertex.position = v0;
    batch3DVertices.push_back(vertex);
    vertex.position = v1;
    batch3DVertices.push_back(vertex);
    vertex.position = v2;
    batch3DVertices.push_back(vertex);
    vertex.position = v3;
    batch3DVertices.push_back(vertex);

    batch3DIndices.push_back(vertexOffset + 0);
    batch3DIndices.push_back(vertexOffset + 1);
    batch3DIndices.push_back(vertexOffset + 2);
    batch3DIndices.push_back(vertexOffset + 2);
    batch3DIndices.push_back(vertexOffset + 3);
    batch3DIndices.push_back(vertexOffset + 0);

    batch3DStats.lineCount++;
}

void Renderer_Metal::drawRect2D(
    const glm::vec2& position, const glm::vec2& size, const glm::vec4& color, float thickness
) {
    glm::vec2 topLeft = position;
    glm::vec2 topRight = position + glm::vec2(size.x, 0.0f);
    glm::vec2 bottomRight = position + size;
    glm::vec2 bottomLeft = position + glm::vec2(0.0f, size.y);

    drawLine2D(topLeft, topRight, color, thickness);
    drawLine2D(topRight, bottomRight, color, thickness);
    drawLine2D(bottomRight, bottomLeft, color, thickness);
    drawLine2D(bottomLeft, topLeft, color, thickness);
}

void Renderer_Metal::drawCircle2D(const glm::vec2& center, float radius, const glm::vec4& color, int segments) {
    float angleStep = 2.0f * glm::pi<float>() / static_cast<float>(segments);
    for (int i = 0; i < segments; ++i) {
        float angle0 = angleStep * i;
        float angle1 = angleStep * (i + 1);

        glm::vec2 p0 = center + glm::vec2(std::cos(angle0) * radius, std::sin(angle0) * radius);
        glm::vec2 p1 = center + glm::vec2(std::cos(angle1) * radius, std::sin(angle1) * radius);

        drawLine2D(p0, p1, color, 1.0f);
    }
    batch2DStats.circleCount++;
}

void Renderer_Metal::drawCircleFilled2D(const glm::vec2& center, float radius, const glm::vec4& color, int segments) {
    float angleStep = 2.0f * glm::pi<float>() / static_cast<float>(segments);

    for (int i = 0; i < segments; ++i) {
        float angle0 = angleStep * i;
        float angle1 = angleStep * (i + 1);

        glm::vec2 p0 = center;
        glm::vec2 p1 = center + glm::vec2(std::cos(angle0) * radius, std::sin(angle0) * radius);
        glm::vec2 p2 = center + glm::vec2(std::cos(angle1) * radius, std::sin(angle1) * radius);

        drawTriangleFilled2D(p0, p1, p2, color);
    }
    batch2DStats.circleCount++;
}

void Renderer_Metal::drawTriangle2D(
    const glm::vec2& p0, const glm::vec2& p1, const glm::vec2& p2, const glm::vec4& color
) {
    drawLine2D(p0, p1, color, 1.0f);
    drawLine2D(p1, p2, color, 1.0f);
    drawLine2D(p2, p0, color, 1.0f);
}

void Renderer_Metal::drawTriangleFilled2D(
    const glm::vec2& p0, const glm::vec2& p1, const glm::vec2& p2, const glm::vec4& color
) {
    if (batch2DIndices.size() >= BatchMaxIndices) return;

    glm::vec2 defaultUV(0.5f, 0.5f);
    Uint32 vertexOffset = static_cast<Uint32>(batch2DVertices.size());

    Batch2DVertex vertex;
    vertex.color = color;
    vertex.uv = defaultUV;
    vertex.texIndex = 0.0f;
    vertex.entityID = -1.0f;

    vertex.position = glm::vec3(p0, 0.0f);
    batch2DVertices.push_back(vertex);
    vertex.position = glm::vec3(p1, 0.0f);
    batch2DVertices.push_back(vertex);
    vertex.position = glm::vec3(p2, 0.0f);
    batch2DVertices.push_back(vertex);
    // Degenerate 4th vertex
    vertex.position = glm::vec3(p2, 0.0f);
    batch2DVertices.push_back(vertex);

    batch2DIndices.push_back(vertexOffset + 0);
    batch2DIndices.push_back(vertexOffset + 1);
    batch2DIndices.push_back(vertexOffset + 2);
    batch2DIndices.push_back(vertexOffset + 2);
    batch2DIndices.push_back(vertexOffset + 3);
    batch2DIndices.push_back(vertexOffset + 0);

    batch2DStats.triangleCount++;
}

// Helper function to get Metal device without including renderer_metal.hpp in main.cpp
// Takes void* to avoid needing Renderer_Metal definition in caller
extern "C" void* getMetalDevice(void* renderer) {
    if (renderer) {
        Renderer_Metal* metalRenderer = static_cast<Renderer_Metal*>(renderer);
        return static_cast<void*>(metalRenderer->getDevice());
    }
    return nullptr;
}