#pragma once

#include "rhi.hpp"

#include <SDL3/SDL_video.h>
#include <SDL3/SDL_render.h>
#include <Foundation/Foundation.hpp>
#include <Metal/Metal.hpp>
#include <QuartzCore/QuartzCore.hpp>
#include <unordered_map>
#include <vector>

// ============================================================================
// RHI_Metal - Metal Implementation of RHI Interface
// ============================================================================

class RHI_Metal : public RHI {
public:
    RHI_Metal();
    ~RHI_Metal() override;

    // ========================================================================
    // Initialization
    // ========================================================================

    bool initialize(SDL_Window* window) override;
    void shutdown() override;
    void waitIdle() override;

    // ========================================================================
    // Resource Creation
    // ========================================================================

    BufferHandle createBuffer(const BufferDesc& desc) override;
    void destroyBuffer(BufferHandle handle) override;

    TextureHandle createTexture(const TextureDesc& desc) override;
    void destroyTexture(TextureHandle handle) override;

    ShaderHandle createShader(const ShaderDesc& desc) override;
    void destroyShader(ShaderHandle handle) override;

    SamplerHandle createSampler(const SamplerDesc& desc) override;
    void destroySampler(SamplerHandle handle) override;

    PipelineHandle createPipeline(const PipelineDesc& desc) override;
    void destroyPipeline(PipelineHandle handle) override;

    ComputePipelineHandle createComputePipeline(const ComputePipelineDesc& desc) override;
    void destroyComputePipeline(ComputePipelineHandle handle) override;

    AccelStructHandle createAccelerationStructure(const AccelStructDesc& desc) override;
    void destroyAccelerationStructure(AccelStructHandle handle) override;
    void buildAccelerationStructure(AccelStructHandle handle) override;
    void updateAccelerationStructure(AccelStructHandle handle, const std::vector<AccelStructInstance>& instances) override;

    // ========================================================================
    // Resource Updates
    // ========================================================================

    void updateBuffer(BufferHandle handle, const void* data, size_t offset, size_t size) override;
    void updateTexture(TextureHandle handle, const void* data, size_t size) override;

    // ========================================================================
    // Frame Operations
    // ========================================================================

    void beginFrame() override;
    void endFrame() override;

    void beginRenderPass(const RenderPassDesc& desc) override;
    void endRenderPass() override;

    // ========================================================================
    // Rendering Commands
    // ========================================================================

    void bindPipeline(PipelineHandle pipeline) override;
    void bindVertexBuffer(BufferHandle buffer, Uint32 binding, size_t offset) override;
    void bindIndexBuffer(BufferHandle buffer, size_t offset) override;

    void setUniformBuffer(Uint32 set, Uint32 binding, BufferHandle buffer, size_t offset, size_t range) override;
    void setStorageBuffer(Uint32 set, Uint32 binding, BufferHandle buffer, size_t offset, size_t range) override;
    void setTexture(Uint32 set, Uint32 binding, TextureHandle texture, SamplerHandle sampler) override;

    void draw(Uint32 vertexCount, Uint32 instanceCount, Uint32 firstVertex, Uint32 firstInstance) override;
    void drawIndexed(Uint32 indexCount, Uint32 instanceCount, Uint32 firstIndex, int32_t vertexOffset, Uint32 firstInstance) override;

    // ========================================================================
    // Compute Commands
    // ========================================================================

    void beginComputePass() override;
    void endComputePass() override;
    void bindComputePipeline(ComputePipelineHandle pipeline) override;
    void setComputeBuffer(Uint32 binding, BufferHandle buffer, size_t offset, size_t range) override;
    void setComputeTexture(Uint32 binding, TextureHandle texture) override;
    void setAccelerationStructure(Uint32 binding, AccelStructHandle accelStruct) override;
    void dispatch(Uint32 groupCountX, Uint32 groupCountY, Uint32 groupCountZ) override;

    // ========================================================================
    // Utility
    // ========================================================================

    Uint32 getSwapchainWidth() const override;
    Uint32 getSwapchainHeight() const override;
    PixelFormat getSwapchainFormat() const override;

    // ========================================================================
    // Backend Query Interface
    // ========================================================================

    void* getBackendDevice() const override;
    void* getBackendPhysicalDevice() const override { return nullptr; } // N/A for Metal
    void* getBackendInstance() const override { return nullptr; } // N/A for Metal
    void* getBackendQueue() const override;
    void* getBackendCommandBuffer() const override;

private:
    // ========================================================================
    // Metal Objects
    // ========================================================================

    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    CA::MetalLayer* swapchain = nullptr;
    MTL::Device* device = nullptr;
    NS::SharedPtr<MTL::CommandQueue> commandQueue;

    // Current frame resources
    CA::MetalDrawable* currentDrawable = nullptr;
    NS::SharedPtr<MTL::CommandBuffer> currentCommandBuffer;
    MTL::RenderCommandEncoder* currentRenderEncoder = nullptr;
    MTL::ComputeCommandEncoder* currentComputeEncoder = nullptr;

    // Swapchain properties
    Uint32 swapchainWidth = 0;
    Uint32 swapchainHeight = 0;
    MTL::PixelFormat swapchainFormat = MTL::PixelFormatRGBA8Unorm_sRGB;

    // ========================================================================
    // Resource Storage
    // ========================================================================

    struct BufferResource {
        NS::SharedPtr<MTL::Buffer> buffer;
        size_t size;
        bool isMapped;
        void* mappedPointer;
    };

    struct TextureResource {
        NS::SharedPtr<MTL::Texture> texture;
        Uint32 width;
        Uint32 height;
        Uint32 depth;
        Uint32 mipLevels;
        MTL::PixelFormat format;
    };

    struct ShaderResource {
        NS::SharedPtr<MTL::Library> library;
        NS::SharedPtr<MTL::Function> function;
        ShaderStage stage;
    };

    struct SamplerResource {
        NS::SharedPtr<MTL::SamplerState> sampler;
    };

    struct PipelineResource {
        NS::SharedPtr<MTL::RenderPipelineState> renderPipeline;
        NS::SharedPtr<MTL::ComputePipelineState> computePipeline;
        bool isCompute;
    };

    struct ComputePipelineResource {
        NS::SharedPtr<MTL::ComputePipelineState> pipeline;
    };

    struct AccelStructResource {
        NS::SharedPtr<MTL::AccelerationStructure> accelStruct;
        NS::SharedPtr<MTL::Buffer> scratchBuffer;
        AccelStructType type;
        std::vector<AccelStructGeometry> geometries;
        std::vector<AccelStructInstance> instances;
    };

    // Resource maps
    Uint32 nextBufferId = 1;
    Uint32 nextTextureId = 1;
    Uint32 nextShaderId = 1;
    Uint32 nextSamplerId = 1;
    Uint32 nextPipelineId = 1;
    Uint32 nextComputePipelineId = 1;
    Uint32 nextAccelStructId = 1;

    std::unordered_map<Uint32, BufferResource> buffers;
    std::unordered_map<Uint32, TextureResource> textures;
    std::unordered_map<Uint32, ShaderResource> shaders;
    std::unordered_map<Uint32, SamplerResource> samplers;
    std::unordered_map<Uint32, PipelineResource> pipelines;
    std::unordered_map<Uint32, ComputePipelineResource> computePipelines;
    std::unordered_map<Uint32, AccelStructResource> accelStructs;

    // Current binding state
    PipelineHandle currentPipeline = {0};
    ComputePipelineHandle currentComputePipeline = {0};
    BufferHandle currentVertexBuffer = {0};
    BufferHandle currentIndexBuffer = {0};

    // ========================================================================
    // Internal Helpers
    // ========================================================================

    MTL::PixelFormat convertPixelFormat(PixelFormat format);
    MTL::TextureUsage convertTextureUsage(Uint32 usage);
    MTL::SamplerAddressMode convertSamplerAddressMode(SamplerAddressMode mode);
    MTL::SamplerMinMagFilter convertSamplerFilter(SamplerFilter filter);
    MTL::SamplerMipFilter convertSamplerMipFilter(SamplerFilter filter);
    MTL::CompareFunction convertCompareOp(CompareOp op);
    MTL::PrimitiveType convertPrimitiveTopology(PrimitiveTopology topology);
    MTL::CullMode convertCullMode(CullMode mode);
    MTL::Winding convertFrontFace(bool counterClockwise);
    MTL::BlendFactor convertBlendFactor(BlendMode mode, bool isSource, bool isAlpha);
    MTL::BlendOperation convertBlendOp();

    NS::SharedPtr<MTL::Function> createShaderFunction(const std::string& source, ShaderStage stage);
    NS::SharedPtr<MTL::RenderPipelineState> createRenderPipeline(const PipelineDesc& desc);
};
