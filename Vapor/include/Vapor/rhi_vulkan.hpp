#pragma once
#include "rhi.hpp"
#include <SDL3/SDL_video.h>
#include <SDL3/SDL_vulkan.h>
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_beta.h>
#include <vk_mem_alloc.h>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <vector>
#include <unordered_map>

// ============================================================================
// RHI_Vulkan - Vulkan implementation of RHI interface
// ============================================================================

class RHI_Vulkan : public RHI {
public:
    RHI_Vulkan();
    ~RHI_Vulkan() override;

    // ========================================================================
    // Initialization
    // ========================================================================

    bool initialize(SDL_Window* window) override;
    void shutdown() override;
    void waitIdle() override;

    // Raytracing (VK_KHR_acceleration_structure) is not implemented — passes
    // that require it are skipped by the RenderGraph on this backend.
    const RHICapabilities& getCapabilities() const override { return capabilities; }

    Uint32 getMaxFramesInFlight() const override { return MAX_FRAMES_IN_FLIGHT; }

    // GPU per-pass timing via vkCmdWriteTimestamp (see Metal backend for the
    // shared semantics: results lag by MAX_FRAMES_IN_FLIGHT frames)
    bool isGpuTimingSupported() const override { return gpuTimingSupported; }
    void setGpuTimingEnabled(bool enabled) override { gpuTimingEnabled = enabled; }
    bool isGpuTimingEnabled() const override { return gpuTimingEnabled; }
    std::vector<GpuPassTiming> getGpuPassTimings() override;
    double getGpuFrameSpanMs() override;
    double getGpuFrameBusyMs() override;

    // ========================================================================
    // Resource Creation
    // ========================================================================

    BufferHandle createBuffer(const BufferDesc& desc) override;
    void destroyBuffer(BufferHandle handle) override;

    TextureHandle createTexture(const TextureDesc& desc) override;
    TextureHandle createTextureView(const TextureViewDesc& desc) override;
    void destroyTexture(TextureHandle handle) override;

    ShaderHandle createShader(const ShaderDesc& desc) override;
    void destroyShader(ShaderHandle handle) override;

    SamplerHandle createSampler(const SamplerDesc& desc) override;
    void destroySampler(SamplerHandle handle) override;

    PipelineHandle createPipeline(const PipelineDesc& desc) override;
    PipelineHandle createMeshPipeline(const MeshPipelineDesc& desc) override;
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
    void copyTexture(TextureHandle src, Uint32 srcMip, TextureHandle dst, Uint32 dstMip) override;
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
    void drawIndirect(BufferHandle argsBuffer, size_t offset, Uint32 drawCount, Uint32 stride) override;
    void drawMeshTasks(Uint32 groupCountX, Uint32 groupCountY = 1, Uint32 groupCountZ = 1) override;

    // Bindless texture tables (Bindless MDI): descriptor-indexed set 3.
    BufferHandle createTextureArgumentTable(ShaderHandle fragmentShader, Uint32 bufferIndex,
                                            Uint32 entryCount, Uint32 texturesPerEntry) override;
    void writeTextureArgumentTable(BufferHandle table, Uint32 entry, Uint32 slot,
                                   TextureHandle texture) override;
    void bindTextureArgumentTable(BufferHandle table) override;

    // ========================================================================
    // Compute Commands
    // ========================================================================

    void beginComputePass(const char* name = "Compute") override;
    void endComputePass() override;
    void bindComputePipeline(ComputePipelineHandle pipeline) override;
    void setComputeBuffer(Uint32 binding, BufferHandle buffer, size_t offset, size_t range) override;
    void setComputeTexture(Uint32 binding, TextureHandle texture) override;
    void setComputeSampledTexture(Uint32 binding, TextureHandle texture, SamplerHandle sampler) override;
    void setComputeBytes(const void* data, size_t size, Uint32 binding) override;
    void setAccelerationStructure(Uint32 binding, AccelStructHandle accelStruct) override;
    void dispatch(Uint32 groupCountX, Uint32 groupCountY, Uint32 groupCountZ) override;
    void computeBarrier() override;
    void setScissor(int32_t x, int32_t y, Uint32 width, Uint32 height) override;

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
    void* getBackendTexture(TextureHandle handle) const override;
    void* getBackendSampler(SamplerHandle handle) const override;
    void* getBackendPhysicalDevice() const override;
    void* getBackendInstance() const override;
    void* getBackendQueue() const override;
    void* getBackendCommandBuffer() const override;

private:
    // ========================================================================
    // Vulkan Objects
    // ========================================================================

    SDL_Window* window = nullptr;
    RHICapabilities capabilities;  // filled in initialize(); raytracing stays false
    VkInstance instance = VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    // Single VMA allocator for every buffer/image: sub-allocates from large
    // device memory blocks instead of one vkAllocateMemory per resource (which
    // fragments and runs into maxMemoryAllocationCount, often just 4096).
    VmaAllocator allocator = VK_NULL_HANDLE;
    VkQueue graphicsQueue = VK_NULL_HANDLE;
    VkQueue presentQueue = VK_NULL_HANDLE;
    Uint32 graphicsFamilyIdx = UINT32_MAX;
    Uint32 presentFamilyIdx = UINT32_MAX;

    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkFormat swapchainImageFormat;
    VkExtent2D swapchainExtent;
    // Extent of the render pass currently being encoded (for scissor clamping).
    Uint32 currentPassWidth = 0;
    Uint32 currentPassHeight = 0;
    std::vector<VkImage> swapchainImages;
    std::vector<VkImageView> swapchainImageViews;

    VkCommandPool commandPool = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> commandBuffers;

    std::vector<VkSemaphore> imageAvailableSemaphores;
    std::vector<VkSemaphore> renderFinishedSemaphores;
    std::vector<VkFence> inFlightFences;

    // Current frame. This is the backend's own frames-in-flight and the value
    // getMaxFramesInFlight() returns; the renderer derives its per-frame buffer
    // slot count from it, so one slot per in-flight frame is guaranteed. Kept at
    // 3 to match the Metal backend.
    const Uint32 MAX_FRAMES_IN_FLIGHT = 3;
    Uint32 currentFrameInFlight = 0;
    Uint32 currentSwapchainImageIndex = 0;
    VkCommandBuffer currentCommandBuffer = VK_NULL_HANDLE;
    PipelineHandle currentPipeline;

    // ========================================================================
    // Resource Maps
    // ========================================================================

    struct BufferResource {
        VkBuffer buffer;
        VmaAllocation allocation;
        VkDeviceSize size;
        bool isMapped;
        void* mappedData;
        bool hostVisible = true;
    };

    struct TextureResource {
        VkImage image;
        VkImageView view;
        VmaAllocation allocation;
        VkFormat format;
        Uint32 width;
        Uint32 height;
        Uint32 depth = 1;  // >1 = 3D volume texture
        Uint32 arrayLayers = 1;
        VkImageUsageFlags usage = 0;
        VkImageLayout currentLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        // Lazily-created single-layer views for render-to-array-layer passes
        // (keyed by array layer). Freed alongside the main view.
        std::unordered_map<Uint32, VkImageView> layerViews;
    };

    struct ShaderResource {
        VkShaderModule module;
        ShaderStage stage;
    };

    struct SamplerResource {
        VkSampler sampler;
    };

    struct PipelineResource {
        VkPipeline pipeline;
        VkPipelineLayout layout;
    };

    struct ComputePipelineResource {
        VkPipeline pipeline;
        VkPipelineLayout layout;
    };

    struct AccelStructResource {
        // Vulkan ray tracing is not implemented yet
        // Will use VK_KHR_acceleration_structure when needed
        VkBuffer scratchBuffer;
        VkDeviceMemory scratchMemory;
        void* buildData;
    };

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

    // VAPOR_RHI_STATS=1 leak-hunt instrumentation (see beginFrame): per-frame
    // counters reported every ~120 frames, then reset.
    Uint32 statsDescriptorSets = 0;   // sets allocated since last report
    Uint32 statsBufferCreates = 0;    // createBuffer calls since last report
    Uint32 statsTextureCreates = 0;   // createTexture calls since last report
    VkDeviceSize statsStagingHighWater = 0;

    // ========================================================================
    // Vulkan Extension Function Pointers
    // ========================================================================

    // Dynamic rendering extension functions
    PFN_vkCmdBeginRenderingKHR vkCmdBeginRenderingKHR = nullptr;
    PFN_vkCmdEndRenderingKHR vkCmdEndRenderingKHR = nullptr;

    // ========================================================================
    // Descriptor Binding Model
    // ------------------------------------------------------------------------
    // The RHI exposes Metal-style stage-indexed bindings. On Vulkan they map
    // onto one global pipeline layout shared by every graphics pipeline:
    //   set 0 = vertex-stage buffers   (setVertexBuffer)    — SSBO, 8 slots
    //   set 1 = fragment-stage buffers (setFragmentBuffer)  — SSBO, 8 slots
    //   set 2 = fragment textures      (setTexture)         — sampler, 8 slots
    //   push constants (128 B): vertex bytes at (binding%4)*16 in [0,64),
    //                           fragment bytes at 64+(binding%4)*16 in [64,128)
    // Bindings accumulate in CPU-side state; a fresh descriptor set trio is
    // allocated from the per-frame pool and (re)bound whenever a draw happens
    // after a binding change.
    // ========================================================================

    // Buffer + storage-image sets stay at 8: MoltenVK/Metal caps
    // maxPerStageDescriptorStorageImages (and storage buffers) at 8, so bumping
    // these would fail pipeline-layout creation. The meshlet vertex-stage binds
    // (slots 0-7) fit exactly.
    static constexpr Uint32 BINDINGS_PER_SET = 8;
    // The texture set (combined image samplers, device limit >= 16) holds the 10
    // material/shadow/AO/SSCS/near samplers (b0-b9) plus the 3 IBL maps the main
    // pass now samples: irradiance cube (b10), prefilter cube (b11), BRDF LUT
    // (b12). Additive capacity: the write loop only writes bound slots, so
    // pipelines that use fewer textures (bloom, IBL bake) are unaffected.
    // (Bindless MDI's material texture array lives in set 3, not here.)
    static constexpr Uint32 TEXTURE_BINDINGS_PER_SET = 13;

    struct BufferBinding {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceSize offset = 0;
        VkDeviceSize range = VK_WHOLE_SIZE;
    };
    struct TextureBinding {
        VkImageView view = VK_NULL_HANDLE;
        VkSampler sampler = VK_NULL_HANDLE;
    };

    VkDescriptorSetLayout vertexBufferSetLayout = VK_NULL_HANDLE;   // set 0
    VkDescriptorSetLayout fragmentBufferSetLayout = VK_NULL_HANDLE; // set 1
    VkDescriptorSetLayout textureSetLayout = VK_NULL_HANDLE;        // set 2
    VkPipelineLayout globalPipelineLayout = VK_NULL_HANDLE;
    std::vector<VkDescriptorPool> descriptorPools;  // one per frame in flight

    // Bindless texture tables (Bindless MDI): set 3 in the global layout when
    // descriptor indexing is enabled — binding 0 = runtime sampled-image array
    // (partially bound, update-after-bind), binding 1 = one linear-repeat
    // sampler. Tables are persistent update-after-bind sets from their own pool.
    bool descriptorIndexingEnabled = false;
    static constexpr Uint32 BINDLESS_TABLE_CAPACITY = 30720;  // >= MAX_INSTANCES * 6
    VkDescriptorSetLayout bindlessSetLayout = VK_NULL_HANDLE;  // set 3
    VkDescriptorPool bindlessPool = VK_NULL_HANDLE;
    VkSampler bindlessSampler = VK_NULL_HANDLE;
    struct BindlessTableResource {
        VkDescriptorSet set = VK_NULL_HANDLE;
        Uint32 entryCount = 0;
        Uint32 texturesPerEntry = 0;
    };
    // Keyed by the opaque BufferHandle id returned from createTextureArgumentTable
    // (no VkBuffer behind it — binding goes through vkCmdBindDescriptorSets).
    std::unordered_map<Uint32, BindlessTableResource> bindlessTables;

    BufferBinding boundVertexBuffers[BINDINGS_PER_SET];
    BufferBinding boundFragmentBuffers[BINDINGS_PER_SET];
    TextureBinding boundTextures[TEXTURE_BINDINGS_PER_SET];
    bool descriptorsDirty = true;

    // Compute follows the same model with its own global layout:
    //   set 0 = storage buffers  (setComputeBuffer)
    //   set 1 = storage images   (setComputeTexture, transitioned to GENERAL, imageLoad)
    //   set 2 = sampled textures (setComputeSampledTexture, SHADER_READ, textureLod)
    VkDescriptorSetLayout computeBufferSetLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout computeImageSetLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout computeSampledSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout computePipelineLayout = VK_NULL_HANDLE;
    // VK_EXT_mesh_shader: set during device creation when the extension + its
    // task/mesh features are supported and enabled. Widens the graphics stage
    // mask (set layouts + push constants) so task/mesh stages can bind through
    // the existing setVertexBuffer/setVertexBytes paths.
    bool meshShadersEnabled = false;
    VkShaderStageFlags graphicsStageFlags = 0;
    PFN_vkCmdDrawMeshTasksEXT pfnCmdDrawMeshTasks = nullptr;
    BufferBinding boundComputeBuffers[BINDINGS_PER_SET];
    VkImageView boundComputeImages[BINDINGS_PER_SET] = {};
    TextureBinding boundComputeSampled[BINDINGS_PER_SET] = {};
    bool computeDescriptorsDirty = true;
    void flushComputeDescriptors();

    // ========================================================================
    // Batched Upload Stream
    // ------------------------------------------------------------------------
    // Uploads to GPU-only resources are recorded into a dedicated command
    // buffer through a persistently-mapped staging ring and submitted lazily:
    // at beginFrame (same-queue submission order makes the data visible to
    // that frame without a CPU wait), on flushUploads(), or when the ring
    // wraps (which waits for prior upload submissions to reclaim space).
    // ========================================================================

    static constexpr VkDeviceSize STAGING_RING_SIZE = 32ull * 1024 * 1024;
    VkBuffer stagingRingBuffer = VK_NULL_HANDLE;
    VmaAllocation stagingRingAllocation = VK_NULL_HANDLE;
    void* stagingRingPtr = nullptr;  // persistently mapped via VMA_ALLOCATION_CREATE_MAPPED_BIT
    // Frame-partitioned staging: the ring is split into MAX_FRAMES_IN_FLIGHT
    // equal regions. Each frame allocates only within its own region [base,
    // base+regionSize); the region is reset at beginFrame right after
    // inFlightFences[slot] is waited, which provably completes that slot's
    // previous upload copies. This makes the ring reuse deterministic and
    // stall-free (no dependency on ALL upload fences draining).
    VkDeviceSize stagingRegionBase = 0;   // this frame's region start
    VkDeviceSize stagingRingOffset = 0;   // offset WITHIN the current region
    VkCommandBuffer uploadCmd = VK_NULL_HANDLE;   // valid while recording
    std::vector<VkFence> pendingUploadFences;     // one per in-flight upload submit

    void createUploadStream();
    void destroyUploadStream();
    // Begin/continue recording; returns the open upload command buffer.
    VkCommandBuffer ensureUploadCmd();
    // Reserve `size` staging bytes; returns mapped pointer + buffer offset.
    // May flush (and wait) when the ring wraps.
    void* allocStaging(VkDeviceSize size, VkDeviceSize& outOffset);
    // Copy `data` into staging memory: the ring for normal sizes, a dedicated
    // one-shot buffer (deferred-destroyed) when larger than the whole ring.
    VkBuffer stageData(const void* data, VkDeviceSize size, VkDeviceSize& outOffset);
    // Submit recorded uploads. waitForCompletion also reclaims ring space.
    void submitUploads(bool waitForCompletion);

    // ========================================================================
    // Deferred Destruction
    // ------------------------------------------------------------------------
    // destroyX() removes the handle immediately but the Vulkan objects are
    // destroyed only once every frame that could reference them has finished
    // (frameCounter advances at endFrame; entries retire after
    // MAX_FRAMES_IN_FLIGHT further frames complete).
    // ========================================================================

    Uint64 frameCounter = 0;
    // Throttles the descriptor-pool-exhausted warning to once per frame (else a
    // saturated pool spams every remaining draw). ~0 = never warned.
    Uint64 lastDescExhaustFrame = ~0ull;
    std::deque<std::pair<Uint64, std::function<void()>>> retirementQueue;
    void deferDestroy(std::function<void()> destroy);
    void processRetirements(bool force);

    // ========================================================================
    // GPU Pass Timing (vkCmdWriteTimestamp, per frame-in-flight query pools)
    // ========================================================================

    static constexpr Uint32 TIMESTAMP_QUERIES_PER_POOL = 64;
    struct PassTimestampInfo {
        std::string name;
        Uint32 beginQuery;
        Uint32 endQuery;
    };
    std::vector<VkQueryPool> timestampPools;                    // per frame in flight
    std::vector<std::vector<PassTimestampInfo>> slotTimestamps; // per frame in flight
    Uint32 nextTimestampQuery = 0;
    float timestampPeriodNs = 0.0f;
    bool gpuTimingSupported = false;
    bool gpuTimingEnabled = false;
    // Portability-subset (MoltenVK) may forbid non-identity image-view swizzle;
    // createTextureView falls back to identity when this is false. Default true
    // for full Vulkan (no portability subset present).
    bool imageViewSwizzleSupported = true;
    bool gpuTimingActiveThisFrame = false;
    std::mutex gpuTimingMutex;
    std::vector<GpuPassTiming> gpuPassTimings;
    double gpuFrameSpanMs = 0.0;   // min(begin)->max(end) across passes: latency
    double gpuFrameBusyMs = 0.0;   // interval union of [begin,end] windows: occupancy
    void createTimestampPools();
    void collectTimestamps(Uint32 slot);  // read completed results for a slot
    bool allocateTimestampPair(const char* passName, Uint32& outBegin, Uint32& outEnd);

    // ========================================================================
    // Debug Labels (VK_EXT_debug_utils, optional)
    // ========================================================================

    PFN_vkCmdBeginDebugUtilsLabelEXT pfnCmdBeginDebugLabel = nullptr;
    PFN_vkCmdEndDebugUtilsLabelEXT pfnCmdEndDebugLabel = nullptr;
    bool renderPassLabelOpen = false;
    Uint32 currentPassEndQuery = UINT32_MAX;  // pending end-timestamp for the open pass
    void beginDebugLabel(VkCommandBuffer cmd, const char* name);
    void endDebugLabel(VkCommandBuffer cmd);

    // Swapchain image layout tracking (reset each frame)
    VkImageLayout swapchainImageLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    // Attachments of the render pass currently being recorded (for the
    // attachment -> shader-read transition at endRenderPass)
    std::vector<Uint32> currentPassColorTextures;
    // Depth attachment of the current pass, if it is sampleable (moved to
    // shader-read at endRenderPass so a later pass can sample it, e.g. shadows).
    Uint32 currentPassDepthTexture = 0;

    // Get (or lazily create) a single-array-layer depth view for render-to-layer.
    VkImageView getDepthLayerView(TextureResource& tex, Uint32 layer);
    // Generalized single-layer/mip view (color or depth) for render-to-layer.
    VkImageView getSubresourceView(TextureResource& tex, Uint32 layer, Uint32 mip,
                                   VkImageAspectFlags aspect);

    // Swapchain recreation (window resize / OUT_OF_DATE / SUBOPTIMAL). Marked
    // dirty by acquire/present results, executed at the top of beginFrame.
    bool swapchainDirty = false;
    void recreateSwapchain();

    void createDescriptorInfrastructure();
    void destroyDescriptorInfrastructure();
    void flushDescriptors();
    void transitionImage(VkImage image, VkImageLayout from, VkImageLayout to, VkImageAspectFlags aspect);

    // ========================================================================
    // Internal Helpers
    // ========================================================================

    void createInstance();
    void createSurface();
    void pickPhysicalDevice();
    void createLogicalDevice();
    void createSwapchain();
    void createCommandPool();
    void createCommandBuffers();
    void createSyncObjects();

    VkFormat convertPixelFormat(PixelFormat format);
    VkFilter convertFilterMode(FilterMode mode);
    VkSamplerAddressMode convertAddressMode(AddressMode mode);
    VkCompareOp convertCompareOp(CompareOp op);
    VkPrimitiveTopology convertPrimitiveTopology(PrimitiveTopology topology);
    VkCullModeFlags convertCullMode(CullMode mode);
    VkBufferUsageFlags convertBufferUsage(BufferUsage usage);
    VkImageUsageFlags convertTextureUsage(TextureUsage usage);
};

// Factory function
RHI* createRHIVulkan();
