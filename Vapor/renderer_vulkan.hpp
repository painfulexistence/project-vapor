#pragma once
#include "renderer.hpp"
#include "graphics.hpp"

#include "SDL3/SDL.h"
#include "SDL3/SDL_vulkan.h"
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_beta.h>
#include "glm/mat4x4.hpp"

struct UniformBufferMVP {
    glm::mat4 model;
    glm::mat4 view;
    glm::mat4 proj;
};

struct SceneData {
    glm::vec3 camPos;
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

    virtual void draw() override;

    VkPipeline createPipeline(const std::string& filename);

    VkShaderModule createShaderModule(const std::vector<char>&);

    VkImage createRenderTarget(GPUImageUsage usage, VkDeviceMemory& memory, VkImageView& imageView, int sampleCount);

    VkImage createTexture(const std::string& filename, VkDeviceMemory& memory, VkImageView& imageView);

    VkBuffer createBuffer(GPUBufferUsage usage, VkDeviceSize size, VkDeviceMemory& memory);

    VkBuffer createBufferMapped(GPUBufferUsage usage, VkDeviceSize size, VkDeviceMemory& memory, void** mappedDataPtr);

    VkBuffer createVertexBuffer(std::vector<VertexData> vertices, VkDeviceMemory& bufferMemory);

    VkBuffer createIndexBuffer(std::vector<uint16_t> indices, VkDeviceMemory& bufferMemory);

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
    VkDescriptorPool descriptorPool;
    VkDescriptorSetLayout descriptorSetLayout;
    std::vector<VkDescriptorSet> descriptorSets;
    std::shared_ptr<Mesh> testMesh;

    VkImage colorImage;
    VkDeviceMemory colorImageMemory;
    VkImageView colorImageView;

    VkImage depthImage;
    VkDeviceMemory depthImageMemory;
    VkImageView depthImageView;

    VkImage testAlbedoTexture;
    VkImage testNormalTexture;
    VkImage testAOTexture;
    VkImage testRoughnessTexture;
    VkImage testMetallicTexture;
    VkDeviceMemory testAlbedoTextureMemory;
    VkDeviceMemory testNormalTextureMemory;
    VkDeviceMemory testAOTextureMemory;
    VkDeviceMemory testRoughnessTextureMemory;
    VkDeviceMemory testMetallicTextureMemory;
    VkImageView testAlbedoTextureView;
    VkImageView testNormalTextureView;
    VkImageView testAOTextureView;
    VkImageView testRoughnessTextureView;
    VkImageView testMetallicTextureView;
    VkSampler testSampler;

    VkBuffer testVertexBuffer;
    VkBuffer testIndexBuffer;
    VkDeviceMemory testVertexBufferMemory;
    VkDeviceMemory testIndexBufferMemory;
    std::vector<VkBuffer> uniformBuffers;
    std::vector<VkDeviceMemory> uniformBuffersMemory;
    std::vector<void*> uniformBuffersMapped;

    const int FRAMES_IN_FLIGHT = 3;
    const int sampleCount = 4;
};