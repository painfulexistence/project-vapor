#pragma once

#include "rhi.hpp"

#include <SDL3/SDL_video.h>
#include <SDL3/SDL_render.h>
#include <Foundation/Foundation.hpp>
#include <Metal/Metal.hpp>
#include <QuartzCore/QuartzCore.hpp>
#include <dispatch/dispatch.h>
#include <mutex>
#include <string>
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

    const RHICapabilities& getCapabilities() const override { return capabilities; }

    // CAMetalLayer's drawable pool (3 drawables) provides the CPU throttle
    Uint32 getMaxFramesInFlight() const override { return 3; }

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
    void updateTexture(TextureHandle handle, const void* data, size_t size,
                       Uint32 mipLevel, Uint32 arrayLayer) override;
    using RHI::updateTexture;
    void generateMipmaps(TextureHandle handle) override;
    void flushUploads() override;

    BufferHandle copySwapchainToBuffer(Uint32& outWidth, Uint32& outHeight) override;
    void* mapBuffer(BufferHandle handle) override;
    void unmapBuffer(BufferHandle handle) override;

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

    void setVertexBuffer(Uint32 binding, BufferHandle buffer, size_t offset, size_t range) override;
    void setFragmentBuffer(Uint32 binding, BufferHandle buffer, size_t offset, size_t range) override;

    void setVertexBytes(const void* data, size_t size, Uint32 binding) override;
    void setFragmentBytes(const void* data, size_t size, Uint32 binding) override;

    void draw(Uint32 vertexCount, Uint32 instanceCount, Uint32 firstVertex, Uint32 firstInstance) override;
    void drawIndexed(Uint32 indexCount, Uint32 instanceCount, Uint32 firstIndex, int32_t vertexOffset, Uint32 firstInstance) override;
    void drawIndexedIndirect(BufferHandle argsBuffer, size_t offset, Uint32 drawCount, Uint32 stride) override;

    // ========================================================================
    // Compute Commands
    // ========================================================================

    void beginComputePass(const char* name = "Compute") override;
    void endComputePass() override;
    void bindComputePipeline(ComputePipelineHandle pipeline) override;
    void setComputeBuffer(Uint32 binding, BufferHandle buffer, size_t offset, size_t range) override;
    void setComputeTexture(Uint32 binding, TextureHandle texture) override;
    void setComputeSampledTexture(Uint32 binding, TextureHandle texture, SamplerHandle sampler) override;
    void setAccelerationStructure(Uint32 binding, AccelStructHandle accelStruct) override;
    void setComputeBytes(const void* data, size_t size, Uint32 binding) override;
    void dispatch(Uint32 groupCountX, Uint32 groupCountY, Uint32 groupCountZ) override;
    void setScissor(int32_t x, int32_t y, Uint32 width, Uint32 height) override;

    // ========================================================================
    // Utility
    // ========================================================================

    Uint32 getSwapchainWidth() const override;
    Uint32 getSwapchainHeight() const override;
    PixelFormat getSwapchainFormat() const override;

    // ========================================================================
    // GPU Profiling
    // ========================================================================

    bool isGpuTimingSupported() const override { return gpuTimingSupported; }
    void setGpuTimingEnabled(bool enabled) override { gpuTimingEnabled = enabled; }
    bool isGpuTimingEnabled() const override { return gpuTimingEnabled; }
    std::vector<GpuPassTiming> getGpuPassTimings() override;

    // ========================================================================
    // Backend Query Interface
    // ========================================================================

    void* getBackendDevice() const override;
    void* getBackendTexture(TextureHandle handle) const override;
    void* getBackendPhysicalDevice() const override { return nullptr; } // N/A for Metal
    void* getBackendInstance() const override { return nullptr; } // N/A for Metal
    void* getBackendQueue() const override;
    void* getBackendCommandBuffer() const override;
    MTL::RenderCommandEncoder* getCurrentRenderEncoder() const;
    CA::MetalDrawable* getCurrentDrawable() const;

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
    // Per-frame autorelease pool: metal-cpp returns autoreleased objects from
    // command-buffer / encoder / drawable / NS::String factory calls, and this
    // app has no NSApplicationMain draining one for us. Without a pool bracketing
    // each frame, every encoder (~45/frame), pass-label string, and drawable ref
    // accumulates for the process lifetime — the unbounded RSS growth. Created at
    // the top of beginFrame, drained at the bottom of endFrame.
    NS::AutoreleasePool* framePool = nullptr;
    CA::MetalDrawable* currentDrawable = nullptr;
    NS::SharedPtr<MTL::CommandBuffer> currentCommandBuffer;

    // Explicit CPU throttle, one permit per in-flight frame (== getMaxFramesInFlight()).
    // beginFrame() waits, the frame's completion handler signals. nextDrawable()
    // already bounds the CPU to the drawable pool today, but a frame that never
    // presents (offscreen/compute-only) would not be throttled by it — this makes
    // the bound explicit and matches Vulkan's vkWaitForFences. Balanced on the
    // skipped-frame path so a missing drawable can't drain a permit.
    dispatch_semaphore_t frameSemaphore = nullptr;
    bool frameSemaphoreAcquired = false;
    MTL::RenderCommandEncoder* currentRenderEncoder = nullptr;
    MTL::ComputeCommandEncoder* currentComputeEncoder = nullptr;

    // Swapchain properties
    Uint32 swapchainWidth = 0;
    Uint32 swapchainHeight = 0;
    MTL::PixelFormat swapchainFormat = MTL::PixelFormatRGBA8Unorm_sRGB;
    // Extent of the render pass currently being encoded (for scissor clamping).
    Uint32 currentPassWidth = 0;
    Uint32 currentPassHeight = 0;

    // Device feature support, filled in initialize()
    RHICapabilities capabilities;

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
        Uint32 bytesPerPixel = 4;
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
        bool isCompute = false;
        // Fixed-function state captured from PipelineDesc. Metal has no single
        // pipeline-state object for these, so they are applied at bind time.
        NS::SharedPtr<MTL::DepthStencilState> depthStencilState;
        MTL::CullMode cullMode = MTL::CullModeBack;
        MTL::Winding winding = MTL::WindingCounterClockwise;
        MTL::PrimitiveType primitiveType = MTL::PrimitiveTypeTriangle;
    };

    struct ComputePipelineResource {
        NS::SharedPtr<MTL::ComputePipelineState> pipeline;
        // Threadgroup shape from ComputePipelineDesc — Metal sets it at
        // dispatch time (SPIR-V bakes local_size; MSL does not).
        Uint32 tgX = 1, tgY = 1, tgZ = 1;
    };

    struct AccelStructResource {
        NS::SharedPtr<MTL::AccelerationStructure> accelStruct;
        NS::SharedPtr<MTL::Buffer> scratchBuffer;
        AccelStructType type;
        std::vector<AccelStructGeometry> geometries;
        std::vector<AccelStructInstance> instances;
        // TLAS-only: instance descriptors + the BLAS array they index into.
        NS::SharedPtr<MTL::Buffer> instanceBuffer;
        NS::SharedPtr<NS::Array> blasArray;
        // TLAS-only: per-frame rotation slots (mirrors the native renderer's
        // TLASBuffers[frameInFlight]). Rebuilding a single TLAS in place while
        // an in-flight frame's ray dispatches still traverse it is a GPU-level
        // race (hard-hangs Apple GPUs). accelStruct / scratchBuffer /
        // instanceBuffer above always alias the most recently built slot.
        static constexpr Uint32 kTlasSlots = 3;  // >= max frames in flight
        NS::SharedPtr<MTL::AccelerationStructure> accelSlots[kTlasSlots];
        NS::SharedPtr<MTL::Buffer> scratchSlots[kTlasSlots];
        NS::SharedPtr<MTL::Buffer> instanceSlots[kTlasSlots];
        Uint32 nextSlot = 0;
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

    // Current binding state (invalid = nothing bound)
    PipelineHandle currentPipeline;
    ComputePipelineHandle currentComputePipeline;
    BufferHandle currentVertexBuffer;
    BufferHandle currentIndexBuffer;
    MTL::PrimitiveType currentPrimitiveType = MTL::PrimitiveTypeTriangle;

    // ========================================================================
    // GPU Pass Timing (MTLCounterSampleBuffer timestamps, AtStageBoundary)
    // ========================================================================

    struct PassSampleInfo {
        std::string name;
        NS::UInteger beginIdx;
        NS::UInteger endIdx;
    };

    static constexpr NS::UInteger GPU_TIMER_SAMPLE_COUNT = 64;
    NS::SharedPtr<MTL::CounterSampleBuffer> gpuTimerSampleBuffer;
    std::vector<PassSampleInfo> framePassSamples;   // passes recorded this frame
    NS::UInteger nextTimingSlot = 0;                // next free slot pair
    std::vector<GpuPassTiming> gpuPassTimings;      // last resolved results
    std::mutex gpuTimingMutex;                      // guards gpuPassTimings
    bool gpuTimingSupported = false;
    bool gpuTimingEnabled = false;
    bool gpuTimingActiveThisFrame = false;          // latched at beginFrame

    // ========================================================================
    // Internal Helpers
    // ========================================================================

    MTL::PixelFormat convertPixelFormat(PixelFormat format);
    MTL::TextureUsage convertTextureUsage(TextureUsage usage);
    MTL::SamplerAddressMode convertSamplerAddressMode(AddressMode mode);
    MTL::SamplerMinMagFilter convertSamplerFilter(FilterMode filter);
    MTL::SamplerMipFilter convertSamplerMipFilter(FilterMode filter);
    MTL::CompareFunction convertCompareOp(CompareOp op);
    MTL::PrimitiveType convertPrimitiveTopology(PrimitiveTopology topology);
    MTL::CullMode convertCullMode(CullMode mode);
    MTL::Winding convertFrontFace(bool counterClockwise);

    NS::SharedPtr<MTL::Function> createShaderFunction(const std::string& source, ShaderStage stage);
    NS::SharedPtr<MTL::RenderPipelineState> createRenderPipeline(const PipelineDesc& desc);

    // ========================================================================
    // Batched Upload Stream
    // ------------------------------------------------------------------------
    // Uploads to GPU-only resources are recorded into a dedicated command
    // buffer through a shared staging ring and committed lazily: at
    // beginFrame (queue submission order makes the data visible to that
    // frame), on flushUploads(), or when the ring wraps (which waits on
    // prior upload command buffers to reclaim space).
    //
    // Note on destruction: Metal needs no retirement queue — a committed
    // MTLCommandBuffer retains every resource it references until it
    // completes, so dropping our NS::SharedPtr on destroyX() is safe even
    // with frames in flight.
    // ========================================================================

    static constexpr size_t STAGING_RING_SIZE = 32ull * 1024 * 1024;
    NS::SharedPtr<MTL::Buffer> stagingRingBuffer;
    size_t stagingRingOffset = 0;
    NS::SharedPtr<MTL::CommandBuffer> uploadCmdBuffer;     // open while recording
    MTL::BlitCommandEncoder* uploadBlitEncoder = nullptr;  // open while recording
    std::vector<NS::SharedPtr<MTL::CommandBuffer>> pendingUploadCmds;
    // Oversize (> ring) staging buffers must stay alive until their upload
    // batch completes: the encoder only retains a buffer when the copy is
    // ENCODED, which happens after stageData() returns.
    std::vector<NS::SharedPtr<MTL::Buffer>> oversizeStaging;

    MTL::BlitCommandEncoder* ensureUploadBlit();
    void* allocStaging(size_t size, size_t& outOffset);
    // Copy `data` into staging memory: the ring for normal sizes, a dedicated
    // one-shot buffer (retained by the upload command buffer) when oversize.
    MTL::Buffer* stageData(const void* data, size_t size, size_t& outOffset);
    void submitUploads(bool waitForCompletion);

    // GPU timing helpers
    void initGpuTiming();
    // Reserves a slot pair and records the pass; returns false when timing is
    // off or the per-frame slot budget is exhausted.
    bool allocateTimingSlots(const char* passName, NS::UInteger& outBegin, NS::UInteger& outEnd);
    void resolveGpuTimings();  // installs completion handler on current command buffer
};
