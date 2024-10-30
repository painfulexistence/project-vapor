#pragma once
#include "renderer.hpp"
#include "mesh.hpp"

#include "SDL.h"
#include "SDL_vulkan.h"
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_beta.h>

class Renderer_Vulkan final : Renderer {
public:
    Renderer_Vulkan(SDL_Window* window);

    ~Renderer_Vulkan();

    virtual void init() override;

    virtual void draw() override;

    VkPipeline createPipeline(const std::string& filename);

    VkShaderModule createShaderModule(const std::vector<char>&);

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

    size_t currentFrame = 0;
    std::vector<VkSemaphore> imageAvailableSemaphores;
    std::vector<VkSemaphore> renderFinishedSemaphores;
    std::vector<VkFence> renderFences;

    const int FRAMES_IN_FLIGHT = 3;
};