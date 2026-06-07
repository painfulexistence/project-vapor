#pragma once
#include "renderer_legacy.hpp"

#include <SDL3/SDL_video.h>
#include <SDL3/SDL_stdinc.h>
#include <SDL3/SDL_vulkan.h>
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_beta.h>
#include <glm/mat4x4.hpp>
#include <unordered_map>
#include <memory>

#include "graphics.hpp"

// Handle types (compatible with rhi.hpp but defined locally to avoid BufferUsage conflict)
struct TextureHandle { Uint32 id = UINT32_MAX; };
struct BufferHandle { Uint32 id = UINT32_MAX; };
struct PipelineHandle { Uint32 id = UINT32_MAX; };
struct RenderTargetHandle { Uint32 id = UINT32_MAX; };


class Renderer_Vulkan final : public Renderer {
public:
    Renderer_Vulkan();

    ~Renderer_Vulkan();

    virtual void init(SDL_Window* window) override;

    virtual void deinit() override;

    virtual void stage(std::shared_ptr<Scene> scene) override;

    virtual void draw(std::shared_ptr<Scene> scene, Camera& camera) override;

    virtual void setRenderPath(RenderPath path) override {
        currentRenderPath = path;
    }
    virtual RenderPath getRenderPath() const override {
        return currentRenderPath;
    }

    VkPipeline createPipeline(const std::string& filename1, const std::string& filename2);
    VkPipeline createRenderPipeline(const std::string& vertShader, const std::string& fragShader);
    VkPipeline createPrePassPipeline(const std::string& vertShader, const std::string& fragShader);
    VkPipeline createPostProcessPipeline(const std::string& vertShader, const std::string& fragShader);

    VkPipeline createComputePipeline(const std::string& filename, VkPipelineLayout layout);

    VkShaderModule createShaderModule(const std::string& code);

    RenderTargetHandle createRenderTarget(RenderTargetUsage usage, VkFormat format);

    TextureHandle createTexture(std::shared_ptr<Image> img);

    BufferHandle createBuffer(BufferUsage usage, VkDeviceSize size);

    BufferHandle createBufferMapped(BufferUsage usage, VkDeviceSize size, void** mappedDataPtr);

    BufferHandle createVertexBuffer(std::vector<VertexData> vertices);

    BufferHandle createIndexBuffer(std::vector<Uint32> indices);

    VkBuffer getBuffer(BufferHandle handle) const;
    VkDeviceMemory getBufferMemory(BufferHandle handle) const;

    VkImage getTexture(TextureHandle handle) const;
    VkImageView getTextureView(TextureHandle handle) const;
    VkDeviceMemory getTextureMemory(TextureHandle handle) const;

    VkImage getRenderTarget(RenderTargetHandle handle) const;
    VkImageView getRenderTargetView(RenderTargetHandle handle) const;
    VkDeviceMemory getRenderTargetMemory(RenderTargetHandle handle) const;

    VkPipeline getPipeline(PipelineHandle handle) const;

private:
    VkInstance instance;
    VkSurfaceKHR surface;
    VkPhysicalDevice physicalDevice;
    VkDevice device;
    VkQueue graphicsQueue;
    VkQueue presentQueue;
    Uint32 graphicsFamilyIdx;
    Uint32 presentFamilyIdx;

    VkSwapchainKHR swapchain;
    VkFormat swapchainImageFormat;
    VkExtent2D swapchainExtent;

    std::vector<VkImage> swapchainImages;
    std::vector<VkImageView> swapchainImageViews;

    VkCommandPool commandPool;
    std::vector<VkCommandBuffer> commandBuffers;

    std::vector<VkSemaphore> imageAvailableSemaphores;
    std::vector<VkSemaphore> renderFinishedSemaphores;
    std::vector<VkFence> renderFences;

    VkPipelineLayout renderPipelineLayout;
    VkPipelineLayout prePassPipelineLayout;
    VkPipelineLayout postProcessPipelineLayout;
    VkPipeline renderPipeline;
    VkPipeline prePassPipeline;
    VkPipeline postProcessPipeline;

    VkPipelineLayout tileCullingPipelineLayout;
    VkPipeline tileCullingPipeline;

    VkRenderPass prePass;
    VkRenderPass renderPass;
    VkRenderPass postProcessPass;
    std::vector<VkFramebuffer> prePassFramebuffers;
    std::vector<VkFramebuffer> renderFramebuffers;
    std::vector<VkFramebuffer> postProcessFramebuffers;

    VkDescriptorPool set0DescriptorPool;
    VkDescriptorPool set1DescriptorPool;
    VkDescriptorPool set2DescriptorPool;
    VkDescriptorSetLayout emptySetLayout; // required because VK_EXT_graphics_pipeline_library not supported
    VkDescriptorSetLayout set0Layout;
    VkDescriptorSetLayout set1Layout;
    VkDescriptorSetLayout set2Layout;
    std::vector<VkDescriptorSet> set0s; // global
    std::vector<VkDescriptorSet> set1s; // 1 set per material
    std::vector<VkDescriptorSet> set2s;

    RenderTargetHandle msaaColorImage;
    RenderTargetHandle msaaDepthImage;
    RenderTargetHandle resolveColorImage;

    TextureHandle defaultAlbedoTexture;
    TextureHandle defaultNormalTexture;
    TextureHandle defaultORMTexture;
    TextureHandle defaultEmissiveTexture;
    // TextureHandle defaultDisplacementTexture;
    VkSampler defaultSampler;

    std::vector<BufferHandle> cameraDataBuffers;
    std::vector<void*> cameraDataBuffersMapped;
    std::vector<BufferHandle> instanceDataBuffers;
    std::vector<void*> instanceDataBuffersMapped;
    std::vector<BufferHandle> directionalLightBuffers;
    std::vector<void*> directionalLightBuffersMapped;
    std::vector<BufferHandle> pointLightBuffers;
    std::vector<void*> pointLightBuffersMapped;
    std::vector<BufferHandle> clusterBuffers;
    std::vector<BufferHandle> lightCullDataBuffers;

    std::vector<InstanceData> instances;

    Uint32 nextBufferID = 0;
    Uint32 nextImageID = 0;
    Uint32 nextPipelineID = 0;
    Uint32 nextInstanceID = 0;
    Uint32 nextMaterialID = 0;
    std::unordered_map<Uint32, VkBuffer> buffers;
    std::unordered_map<Uint32, VkDeviceMemory> bufferMemories;
    std::unordered_map<Uint32, VkImage> images;
    std::unordered_map<Uint32, VkDeviceMemory> imageMemories;
    std::unordered_map<Uint32, VkImageView> imageViews;
    std::unordered_map<Uint32, VkPipeline> pipelines;
    std::unordered_map<std::shared_ptr<Material>, VkDescriptorSet> materialTextureSets;
    std::unordered_map<std::shared_ptr<Material>, Uint32> materialIDs;
    // Temporary mapping for Image to TextureHandle (until full refactor to new Renderer interface)
    std::unordered_map<std::shared_ptr<Image>, TextureHandle> imageToTextureMap;
    // Temporary storage for mesh GPU resources (until full refactor to new Renderer interface)
    struct MeshGPUResources {
        std::vector<BufferHandle> vbos;
        BufferHandle ebo;
        Uint32 materialID = UINT32_MAX;
        Uint32 instanceID = UINT32_MAX;
        Uint32 vertexOffset = 0;
        Uint32 indexOffset = 0;
        Uint32 vertexCount = 0;
        Uint32 indexCount = 0;
    };
    std::unordered_map<std::shared_ptr<Mesh>, MeshGPUResources> meshGPUResources;

    RenderPath currentRenderPath = RenderPath::Forward;

    void createResources();
};