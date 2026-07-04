#pragma once
#include "rhi.hpp"
#include <SDL3/SDL_video.h>
#include <SDL3/SDL_vulkan.h>
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_beta.h>
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

    // Raytracing (VK_KHR_acceleration_structure) is not implemented, and
    // compute is unusable until descriptor-set binding lands — passes that
    // require either are skipped by the RenderGraph on this backend.
    const RHICapabilities& getCapabilities() const override { return capabilities; }

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
    void* getBackendPhysicalDevice() const override;
    void* getBackendInstance() const override;
    void* getBackendQueue() const override;
    void* getBackendCommandBuffer() const override;

private:
    // ========================================================================
    // Vulkan Objects
    // ========================================================================

    SDL_Window* window = nullptr;
    RHICapabilities capabilities;  // all false: no RT, no usable compute yet
    VkInstance instance = VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue graphicsQueue = VK_NULL_HANDLE;
    VkQueue presentQueue = VK_NULL_HANDLE;
    Uint32 graphicsFamilyIdx = UINT32_MAX;
    Uint32 presentFamilyIdx = UINT32_MAX;

    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkFormat swapchainImageFormat;
    VkExtent2D swapchainExtent;
    std::vector<VkImage> swapchainImages;
    std::vector<VkImageView> swapchainImageViews;

    VkCommandPool commandPool = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> commandBuffers;

    std::vector<VkSemaphore> imageAvailableSemaphores;
    std::vector<VkSemaphore> renderFinishedSemaphores;
    std::vector<VkFence> inFlightFences;

    // Current frame
    const Uint32 MAX_FRAMES_IN_FLIGHT = 2;
    Uint32 currentFrameInFlight = 0;
    Uint32 currentSwapchainImageIndex = 0;
    VkCommandBuffer currentCommandBuffer = VK_NULL_HANDLE;
    PipelineHandle currentPipeline;

    // ========================================================================
    // Resource Maps
    // ========================================================================

    struct BufferResource {
        VkBuffer buffer;
        VkDeviceMemory memory;
        VkDeviceSize size;
        bool isMapped;
        void* mappedData;
        bool hostVisible = true;
    };

    struct TextureResource {
        VkImage image;
        VkImageView view;
        VkDeviceMemory memory;
        VkFormat format;
        Uint32 width;
        Uint32 height;
        VkImageUsageFlags usage = 0;
        VkImageLayout currentLayout = VK_IMAGE_LAYOUT_UNDEFINED;
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

    static constexpr Uint32 BINDINGS_PER_SET = 8;

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

    BufferBinding boundVertexBuffers[BINDINGS_PER_SET];
    BufferBinding boundFragmentBuffers[BINDINGS_PER_SET];
    TextureBinding boundTextures[BINDINGS_PER_SET];
    bool descriptorsDirty = true;

    // Swapchain image layout tracking (reset each frame)
    VkImageLayout swapchainImageLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    // Attachments of the render pass currently being recorded (for the
    // attachment -> shader-read transition at endRenderPass)
    std::vector<Uint32> currentPassColorTextures;

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

    Uint32 findMemoryType(Uint32 typeFilter, VkMemoryPropertyFlags properties);
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
