#pragma once
#include "rhi.hpp"
#include <SDL3/SDL_video.h>
#include <SDL3/SDL_vulkan.h>
#include <vulkan/vulkan.h>
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

    // ========================================================================
    // Resource Maps
    // ========================================================================

    struct BufferResource {
        VkBuffer buffer;
        VkDeviceMemory memory;
        VkDeviceSize size;
        bool isMapped;
        void* mappedData;
    };

    struct TextureResource {
        VkImage image;
        VkImageView view;
        VkDeviceMemory memory;
        VkFormat format;
        Uint32 width;
        Uint32 height;
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

    Uint32 nextBufferId = 1;
    Uint32 nextTextureId = 1;
    Uint32 nextShaderId = 1;
    Uint32 nextSamplerId = 1;
    Uint32 nextPipelineId = 1;

    std::unordered_map<Uint32, BufferResource> buffers;
    std::unordered_map<Uint32, TextureResource> textures;
    std::unordered_map<Uint32, ShaderResource> shaders;
    std::unordered_map<Uint32, SamplerResource> samplers;
    std::unordered_map<Uint32, PipelineResource> pipelines;

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
