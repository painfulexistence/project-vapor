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

    VkPipeline createPipeline(const std::string& filename);

    VkShaderModule createShaderModule(const std::string& code);

    VkImage createRenderTarget(GPUImageUsage usage, VkDeviceMemory& memory, VkImageView& imageView, int sampleCount);

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
    std::vector<VkFramebuffer> framebuffers;
    VkCommandPool commandPool;
    std::vector<VkCommandBuffer> commandBuffers;

    int currentFrame = 0;
    std::vector<VkSemaphore> imageAvailableSemaphores;
    std::vector<VkSemaphore> renderFinishedSemaphores;
    std::vector<VkFence> renderFences;

    VkPipelineLayout pipelineLayout;
    VkPipeline testDrawPipeline;
    VkRenderPass renderPass;
    VkDescriptorPool frameDescriptorPool;
    VkDescriptorPool instanceDescriptorPool;
    VkDescriptorSetLayout set0Layout;
    VkDescriptorSetLayout set1Layout;
    std::vector<VkDescriptorSet> sets0;
    std::vector<VkDescriptorSet> sets1;

    VkImage colorImage;
    VkDeviceMemory colorImageMemory;
    VkImageView colorImageView;

    VkImage depthImage;
    VkDeviceMemory depthImageMemory;
    VkImageView depthImageView;

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

    const int FRAMES_IN_FLIGHT = 3;
    const int sampleCount = 4;

    Uint32 nextBufferID = 1;
    Uint32 nextTextureID = 1;
    Uint32 nextPipelineID = 1;
    std::unordered_map<Uint32, VkBuffer> buffers;
    std::unordered_map<Uint32, VkDeviceMemory> bufferMemories;
    std::unordered_map<Uint32, VkImage> textures;
    std::unordered_map<Uint32, VkDeviceMemory> textureMemories;
    std::unordered_map<Uint32, VkImageView> textureViews;
    std::unordered_map<Uint32, VkPipeline> pipelines;
    std::unordered_map<Material*, VkDescriptorSet> materialTextureSets; // TODO: better key?
};