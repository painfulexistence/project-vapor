#pragma once
#include "renderer.hpp"

#include <SDL3/SDL_video.h>
#include <SDL3/SDL_stdinc.h>
#include <SDL3/SDL_vulkan.h>
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_beta.h>
#include <glm/mat4x4.hpp>
#include <unordered_map>

#include "graphics.hpp"


struct CameraData {
    glm::mat4 view;
    glm::mat4 proj;
    glm::vec3 pos;
};

struct InstanceData {
    glm::mat4 model;
};

struct SceneData {
    float time;
};

struct Texture {
    VkImage image;
    VkImageView view;
    VkSampler sampler;
    VkDeviceMemory memory;
};

struct Buffer {
    VkBuffer buffer;
    VkDeviceMemory memory;
};

class Renderer_Vulkan final : public Renderer {
public:
    Renderer_Vulkan(SDL_Window* window);

    ~Renderer_Vulkan();

    virtual void init() override;

    virtual void stage(std::shared_ptr<Scene> scene) override;

    virtual void draw(std::shared_ptr<Scene> scene, Camera& camera) override;

    VkPipeline createPipeline(const std::string& filename1, const std::string& filename2);
    VkPipeline createRenderPipeline(const std::string& vertShader, const std::string& fragShader);
    VkPipeline createPrePassPipeline(const std::string& vertShader, const std::string& fragShader);
    VkPipeline createPostProcessPipeline(const std::string& vertShader, const std::string& fragShader);

    VkPipeline createComputePipeline(const std::string& filename);

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

    Uint32 nextBufferID = 1;
    Uint32 nextImageID = 1;
    Uint32 nextPipelineID = 1;
    std::unordered_map<Uint32, VkBuffer> buffers;
    std::unordered_map<Uint32, VkDeviceMemory> bufferMemories;
    std::unordered_map<Uint32, VkImage> images;
    std::unordered_map<Uint32, VkDeviceMemory> imageMemories;
    std::unordered_map<Uint32, VkImageView> imageViews;
    std::unordered_map<Uint32, VkPipeline> pipelines;
    std::unordered_map<Material*, VkDescriptorSet> materialTextureSets; // TODO: better key?
};