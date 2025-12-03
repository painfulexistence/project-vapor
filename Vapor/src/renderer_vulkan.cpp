#include "renderer_vulkan.hpp"

#include <SDL3/SDL_stdinc.h>
#include <SDL3/SDL_timer.h>
#include <fmt/core.h>
#include <tracy/Tracy.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>
#include <cstdlib>
#include <ctime>
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#define GLM_FORCE_LEFT_HANDED
#include "backends/imgui_impl_sdl3.h"
#include "backends/imgui_impl_vulkan.h"
#include <functional>
#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>
#include <vector>

#include "asset_manager.hpp"
#include "graphics.hpp"
#include "helper.hpp"
#define ENABLE_VALIDATION 1
#define USE_DYNAMIC_RENDERING 0


std::unique_ptr<Renderer> createRendererVulkan() {
    return std::make_unique<Renderer_Vulkan>();
}

void insertImageMemoryBarrier(
  VkCommandBuffer cmd,
  VkImage image,
  VkAccessFlags srcAccessMask,
  VkAccessFlags dstAccessMask,
  VkImageLayout currentLayout,
  VkImageLayout newLayout,
  VkPipelineStageFlags srcStageMask,
  VkPipelineStageFlags dstStageMask,
  VkImageSubresourceRange subresourceRange
) {
    VkImageMemoryBarrier imageMemoryBarrier = {};
    imageMemoryBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    imageMemoryBarrier.pNext = nullptr;
    imageMemoryBarrier.srcAccessMask = srcAccessMask;
    imageMemoryBarrier.dstAccessMask = dstAccessMask;
    imageMemoryBarrier.oldLayout = currentLayout;
    imageMemoryBarrier.newLayout = newLayout;
    imageMemoryBarrier.image = image;
    imageMemoryBarrier.subresourceRange = subresourceRange;

    vkCmdPipelineBarrier(cmd, srcStageMask, dstStageMask, 0, 0, nullptr, 0, nullptr, 1, &imageMemoryBarrier);
}

void insertImageMemoryBarrier2(
  VkCommandBuffer cmd,
  VkImage image,
  VkImageLayout currentLayout,
  VkImageLayout newLayout,
  VkImageSubresourceRange subresourceRange
) {
    VkImageMemoryBarrier2 imageMemoryBarrier = {};
    imageMemoryBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    imageMemoryBarrier.pNext = nullptr;
    imageMemoryBarrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    imageMemoryBarrier.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    imageMemoryBarrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    imageMemoryBarrier.dstAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_MEMORY_READ_BIT;
    imageMemoryBarrier.oldLayout = currentLayout;
    imageMemoryBarrier.newLayout = newLayout;
    imageMemoryBarrier.image = image;
    imageMemoryBarrier.subresourceRange = subresourceRange;

    VkDependencyInfo depInfo = {};
    depInfo.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    depInfo.pNext = nullptr;
    depInfo.imageMemoryBarrierCount = 1;
    depInfo.pImageMemoryBarriers = &imageMemoryBarrier;

    vkCmdPipelineBarrier2(cmd, &depInfo);
}

Renderer_Vulkan::Renderer_Vulkan() {
}

Renderer_Vulkan::~Renderer_Vulkan() {
    deinit();
}

auto Renderer_Vulkan::init(SDL_Window* window) -> void {
    ZoneScoped;

    int windowWidth, windowHeight;
    SDL_GetWindowSize(window, &windowWidth, &windowHeight);

    // if (enableValidationLayers && !checkValidationLayerSupport()) {
    //     throw std::runtime_error("Requested validation layers not available!");
    // }
    const std::vector<const char*> validationLayers = {
#if ENABLE_VALIDATION
        "VK_LAYER_KHRONOS_validation"
#endif
    };
    std::vector<const char*> instanceExtensions = { VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME,
                                                    VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME };

    uint32_t instanceExtensionCount;
    const char* const* instanceExtensionNames = SDL_Vulkan_GetInstanceExtensions(&instanceExtensionCount);
    for (uint32_t i = 0; i < instanceExtensionCount; i++) {
        instanceExtensions.emplace_back(instanceExtensionNames[i]);
    }

    const VkApplicationInfo appInfo = {
        VK_STRUCTURE_TYPE_APPLICATION_INFO, nullptr,           "MyApp", VK_MAKE_VERSION(0, 0, 1), "No Engine",
        VK_MAKE_VERSION(0, 0, 1),           VK_API_VERSION_1_3
    };
    const VkInstanceCreateInfo instanceInfo = {
        VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,           nullptr,
        VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR, &appInfo,
        static_cast<uint32_t>(validationLayers.size()),   validationLayers.data(),
        static_cast<uint32_t>(instanceExtensions.size()), instanceExtensions.data(),
    };
    if (vkCreateInstance(&instanceInfo, nullptr, &instance) != VK_SUCCESS) {
        throw std::runtime_error(fmt::format("Failed to create instance! {}", SDL_GetError()));
    }

    if (!SDL_Vulkan_CreateSurface(window, instance, nullptr, &surface)) {
        throw std::runtime_error(fmt::format("Failed to create surface! {}", SDL_GetError()));
    }

    // Find a physical device
    uint32_t physicalDeviceCount;
    vkEnumeratePhysicalDevices(instance, &physicalDeviceCount, nullptr);
    if (physicalDeviceCount == 0) {
        throw std::runtime_error("Failed to find any GPUs with Vulkan support!");
    }
    std::vector<VkPhysicalDevice> physicalDevices(physicalDeviceCount);
    vkEnumeratePhysicalDevices(instance, &physicalDeviceCount, physicalDevices.data());
    // for (const auto &device : physicalDevices) {
    //     VkPhysicalDeviceProperties deviceProperties;
    //     vkGetPhysicalDeviceProperties(device, &deviceProperties);
    //     VkPhysicalDeviceFeatures deviceFeatures;
    //     vkGetPhysicalDeviceFeatures(device, &deviceFeatures);
    //     if (isDeviceSuitable(device)) {
    //         physicalDevice = device;
    //         break;
    //     }
    // }
    physicalDevice = physicalDevices[0];
    if (physicalDevice == VK_NULL_HANDLE) {
        throw std::runtime_error("Failed to find a suitable GPU!");
    }

    // Find one graphics queue family and one present queue family
    uint32_t queueFamilyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &queueFamilyCount, nullptr);
    std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &queueFamilyCount, queueFamilies.data());

    graphicsFamilyIdx = UINT32_MAX;
    presentFamilyIdx = UINT32_MAX;
    uint32_t i = 0;
    for (const auto& queueFamily : queueFamilies) {
        if (graphicsFamilyIdx == UINT32_MAX && queueFamily.queueCount > 0
            && queueFamily.queueFlags & VK_QUEUE_GRAPHICS_BIT)
            graphicsFamilyIdx = i;
        if (presentFamilyIdx == UINT32_MAX) {
            VkBool32 presentSupport;
            vkGetPhysicalDeviceSurfaceSupportKHR(physicalDevice, i, surface, &presentSupport);
            if (presentSupport) presentFamilyIdx = i;
        }
        ++i;
    }
    const uint32_t queueFamilyIndices[2] = { graphicsFamilyIdx, presentFamilyIdx };

    // Create a logical device and get queues
    float queuePriority = 1.0f;
    VkDeviceQueueCreateInfo graphicsQueueInfo = {
        VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, nullptr, 0, graphicsFamilyIdx, 1, &queuePriority,
    };
    VkDeviceQueueCreateInfo presentQueueInfo = {
        VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, nullptr, 0, presentFamilyIdx, 1, &queuePriority,
    };
    VkPhysicalDeviceFeatures deviceFeatures = {
        .samplerAnisotropy = VK_TRUE,
    };
    VkPhysicalDeviceDynamicRenderingFeaturesKHR dynamicRenderingFeatures = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES_KHR,
        .dynamicRendering = VK_TRUE,
    };
    VkPhysicalDeviceSynchronization2Features synchronization2Features = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES,
        .pNext = &dynamicRenderingFeatures,
        .synchronization2 = VK_TRUE,
    };
    const std::vector<const char*> deviceExtensions = {
        VK_KHR_SWAPCHAIN_EXTENSION_NAME,           VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME,
        VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME,   VK_KHR_DEPTH_STENCIL_RESOLVE_EXTENSION_NAME,
        VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME, VK_KHR_MULTIVIEW_EXTENSION_NAME,
        VK_KHR_MAINTENANCE_2_EXTENSION_NAME,       VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME
    };
    VkDeviceCreateInfo deviceInfo = {
        VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        &synchronization2Features,
        0,
        1,
        &graphicsQueueInfo,
        0,
        nullptr,
        static_cast<uint32_t>(deviceExtensions.size()),
        deviceExtensions.data(),
        &deviceFeatures,
    };
    if (graphicsFamilyIdx != presentFamilyIdx) {
        const VkDeviceQueueCreateInfo queueCreateInfos[2] = { graphicsQueueInfo, presentQueueInfo };
        deviceInfo.pQueueCreateInfos = queueCreateInfos;
        deviceInfo.queueCreateInfoCount = 2;
    }
    if (vkCreateDevice(physicalDevice, &deviceInfo, nullptr, &device) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create device!");
    }
    vkGetDeviceQueue(device, graphicsFamilyIdx, 0, &graphicsQueue);
    vkGetDeviceQueue(device, presentFamilyIdx, 0, &presentQueue);

    // Create a swapchain
    VkSurfaceCapabilitiesKHR capabilities;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physicalDevice, surface, &capabilities);
    VkExtent2D extent = capabilities.currentExtent;
    if (capabilities.currentExtent.width == UINT32_MAX) {
        VkExtent2D actualExtent = { static_cast<uint32_t>(windowWidth), static_cast<uint32_t>(windowHeight) };
        actualExtent.width =
          std::max(capabilities.minImageExtent.width, std::min(capabilities.maxImageExtent.width, actualExtent.width));
        actualExtent.height = std::max(
          capabilities.minImageExtent.height, std::min(capabilities.maxImageExtent.height, actualExtent.height)
        );
        extent = actualExtent;
    }
    swapchainExtent = extent;
    std::vector<VkSurfaceFormatKHR> surfaceFormats;
    uint32_t surfaceFormatCount;
    vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, surface, &surfaceFormatCount, nullptr);
    if (surfaceFormatCount != 0) {
        surfaceFormats.resize(surfaceFormatCount);
        vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, surface, &surfaceFormatCount, surfaceFormats.data());
    }
    VkSurfaceFormatKHR selectedSurfaceFormat = surfaceFormats[0];
    for (const auto& surfaceFormat : surfaceFormats) {
        if (surfaceFormat.format == VK_FORMAT_B8G8R8A8_SRGB
            && surfaceFormat.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            selectedSurfaceFormat = surfaceFormat;
            break;
        }
    }
    swapchainImageFormat = selectedSurfaceFormat.format;
    std::vector<VkPresentModeKHR> presentModes;
    uint32_t presentModeCount;
    vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevice, surface, &presentModeCount, nullptr);
    if (presentModeCount != 0) {
        presentModes.resize(presentModeCount);
        vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevice, surface, &presentModeCount, presentModes.data());
    }
    VkPresentModeKHR selectedPresentMode = VK_PRESENT_MODE_FIFO_KHR;
    for (const auto& presentMode : presentModes) {
        if (presentMode == VK_PRESENT_MODE_MAILBOX_KHR) {
            selectedPresentMode = presentMode;
            break;
        }
    }
    uint32_t swapchainImageCount = std::max(capabilities.minImageCount, static_cast<uint32_t>(MAX_FRAMES_IN_FLIGHT));
    if (capabilities.maxImageCount > 0
        && swapchainImageCount > capabilities.maxImageCount) {// Note: maxImageCount = 0 means no maximum
        swapchainImageCount = capabilities.maxImageCount;
    }
    VkSwapchainCreateInfoKHR swapchainInfo = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface = surface,
        .minImageCount = swapchainImageCount,
        .imageFormat = selectedSurfaceFormat.format,
        .imageColorSpace = selectedSurfaceFormat.colorSpace,
        .imageExtent = extent,
        .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .preTransform = capabilities.currentTransform,// Note: capabilities.currentTransform here equals to no transform
        .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        .presentMode = selectedPresentMode,
        .clipped = VK_TRUE,
        .oldSwapchain = VK_NULL_HANDLE,
    };
    if (graphicsFamilyIdx != presentFamilyIdx) {
        swapchainInfo.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
        swapchainInfo.queueFamilyIndexCount = 2;
        swapchainInfo.pQueueFamilyIndices = queueFamilyIndices;
    } else {
        swapchainInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        swapchainInfo.queueFamilyIndexCount = 0;
        swapchainInfo.pQueueFamilyIndices = nullptr;
    }
    if (vkCreateSwapchainKHR(device, &swapchainInfo, nullptr, &swapchain) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create a swapchain!");
    }
    vkGetSwapchainImagesKHR(device, swapchain, &swapchainImageCount, nullptr);
    swapchainImages.resize(swapchainImageCount);
    vkGetSwapchainImagesKHR(device, swapchain, &swapchainImageCount, swapchainImages.data());

    // Create command pools and command buffers
    VkCommandPoolCreateInfo poolInfo = {
        VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        nullptr,
        VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        graphicsFamilyIdx,
    };
    if (vkCreateCommandPool(device, &poolInfo, nullptr, &commandPool) != VK_SUCCESS) {
        throw std::runtime_error("failed to create command pool!");
    }
    commandBuffers.resize(MAX_FRAMES_IN_FLIGHT);
    VkCommandBufferAllocateInfo allocInfo = {};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = commandPool;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = static_cast<uint32_t>(commandBuffers.size());
    if (vkAllocateCommandBuffers(device, &allocInfo, commandBuffers.data()) != VK_SUCCESS) {
        throw std::runtime_error("Failed to allocate command buffers!");
    }

    // Create synchronization objects
    imageAvailableSemaphores.resize(MAX_FRAMES_IN_FLIGHT);
    renderFinishedSemaphores.resize(MAX_FRAMES_IN_FLIGHT);
    renderFences.resize(MAX_FRAMES_IN_FLIGHT);

    VkSemaphoreCreateInfo semaphoreInfo{};
    semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

    for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        if (vkCreateSemaphore(device, &semaphoreInfo, nullptr, &imageAvailableSemaphores[i]) != VK_SUCCESS
            || vkCreateSemaphore(device, &semaphoreInfo, nullptr, &renderFinishedSemaphores[i]) != VK_SUCCESS
            || vkCreateFence(device, &fenceInfo, nullptr, &renderFences[i]) != VK_SUCCESS) {
            throw std::runtime_error("Failed to create semaphores or fences for a frame!");
        }
    }

    // ImGui init
    ImGui_ImplSDL3_InitForVulkan(window);
    ImGui_ImplVulkan_InitInfo initInfo = {
        .ApiVersion = VK_API_VERSION_1_3,
        .Instance = instance,
        .PhysicalDevice = physicalDevice,
        .Device = device,
        .QueueFamily = graphicsFamilyIdx,
        .Queue = graphicsQueue,
        .MinImageCount = static_cast<Uint32>(swapchainImages.size()),
        .ImageCount = static_cast<Uint32>(swapchainImages.size()),
        .MSAASamples = (VkSampleCountFlagBits)MSAA_SAMPLE_COUNT,
        .PipelineCache = VK_NULL_HANDLE,
        .DescriptorPoolSize = 1000,
        .UseDynamicRendering = true,
        .Allocator = nullptr,
        .CheckVkResultFn = nullptr,
    };
    ImGui_ImplVulkan_Init(&initInfo);

    isInitialized = true;
}

auto Renderer_Vulkan::deinit() -> void {
    if (!isInitialized) {
        return;
    }

    // ImGui deinit
    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplSDL3_Shutdown();

    // TODO: clean up all resources
    vkDeviceWaitIdle(device);

    // Cleanup particle system
    cleanupParticleSystem();

    vkDestroyPipeline(device, renderPipeline, nullptr);
    vkDestroyPipeline(device, prePassPipeline, nullptr);
    vkDestroyPipeline(device, postProcessPipeline, nullptr);
    vkDestroyPipeline(device, tileCullingPipeline, nullptr);
    vkDestroyPipelineLayout(device, renderPipelineLayout, nullptr);
    vkDestroyPipelineLayout(device, prePassPipelineLayout, nullptr);
    vkDestroyPipelineLayout(device, postProcessPipelineLayout, nullptr);
    vkDestroyPipelineLayout(device, tileCullingPipelineLayout, nullptr);
    vkDestroyRenderPass(device, renderPass, nullptr);
    vkDestroyRenderPass(device, prePass, nullptr);
    vkDestroyRenderPass(device, postProcessPass, nullptr);
    vkDestroyDescriptorSetLayout(device, emptySetLayout, nullptr);
    vkDestroyDescriptorSetLayout(device, set0Layout, nullptr);
    vkDestroyDescriptorSetLayout(device, set1Layout, nullptr);
    vkDestroyDescriptorSetLayout(device, set2Layout, nullptr);
    vkDestroyDescriptorPool(device, set0DescriptorPool, nullptr);
    vkDestroyDescriptorPool(device, set1DescriptorPool, nullptr);
    vkDestroyDescriptorPool(device, set2DescriptorPool, nullptr);
    vkDestroySampler(device, defaultSampler, nullptr);
    for (auto& image : images) {
        vkDestroyImageView(device, imageViews[image.first], nullptr);
        vkDestroyImage(device, image.second, nullptr);
        vkFreeMemory(device, imageMemories[image.first], nullptr);
    }
    for (auto& buffer : buffers) {
        vkDestroyBuffer(device, buffer.second, nullptr);
        vkFreeMemory(device, bufferMemories[buffer.first], nullptr);
    }
    for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        vkDestroySemaphore(device, renderFinishedSemaphores[i], nullptr);
        vkDestroySemaphore(device, imageAvailableSemaphores[i], nullptr);
        vkDestroyFence(device, renderFences[i], nullptr);
    }
    vkFreeCommandBuffers(device, commandPool, static_cast<uint32_t>(commandBuffers.size()), commandBuffers.data());
    vkDestroyCommandPool(device, commandPool, nullptr);

    for (auto framebuffer : postProcessFramebuffers) {
        vkDestroyFramebuffer(device, framebuffer, nullptr);
    }
    for (auto framebuffer : prePassFramebuffers) {
        vkDestroyFramebuffer(device, framebuffer, nullptr);
    }
    for (auto framebuffer : renderFramebuffers) {
        vkDestroyFramebuffer(device, framebuffer, nullptr);
    }

    for (auto imageView : swapchainImageViews) {
        vkDestroyImageView(device, imageView, nullptr);
    }
    vkDestroySwapchainKHR(device, swapchain, nullptr);
    vkDestroyDevice(device, nullptr);
    vkDestroySurfaceKHR(instance, surface, nullptr);
    vkDestroyInstance(instance, nullptr);

    isInitialized = false;
}

auto Renderer_Vulkan::createResources() -> void {
    // Create swapchain image views
    swapchainImageViews.resize(swapchainImages.size());
    for (size_t i = 0; i < swapchainImages.size(); i++) {
        VkImageViewCreateInfo imageViewInfo{};
        imageViewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        imageViewInfo.image = swapchainImages[i];
        imageViewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        imageViewInfo.format = swapchainImageFormat;
        imageViewInfo.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
        imageViewInfo.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
        imageViewInfo.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
        imageViewInfo.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
        imageViewInfo.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        if (vkCreateImageView(device, &imageViewInfo, nullptr, &swapchainImageViews[i]) != VK_SUCCESS) {
            throw std::runtime_error("Failed to create one of image views!");
        }
    }

    // Create images and views
    msaaColorImage = createRenderTarget(RenderTargetUsage::COLOR_MSAA, VK_FORMAT_R16G16B16A16_SFLOAT);// HDR format
    msaaDepthImage = createRenderTarget(RenderTargetUsage::DEPTH_MSAA, VK_FORMAT_D32_SFLOAT);
    resolveColorImage = createRenderTarget(RenderTargetUsage::COLOR, VK_FORMAT_R16G16B16A16_SFLOAT);// HDR format

#if !(USE_DYNAMIC_RENDERING)
    // Create render passes
    std::array<VkAttachmentDescription, 1> prePassAttachments = { { {
      .format = VK_FORMAT_D32_SFLOAT,
      .samples = (VkSampleCountFlagBits)MSAA_SAMPLE_COUNT,
      .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
      .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
      .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
      .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
      .finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
    } } };

    VkAttachmentReference prePassDepthAttachmentRef = {};
    prePassDepthAttachmentRef.attachment = 0;// index in attachment descriptions array, also index in shader layout
    prePassDepthAttachmentRef.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkSubpassDescription prePassSubpass = {};
    prePassSubpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    prePassSubpass.colorAttachmentCount = 0;
    prePassSubpass.pDepthStencilAttachment = &prePassDepthAttachmentRef;

    VkSubpassDependency prePassDependency = {};
    prePassDependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    prePassDependency.dstSubpass = 0;
    prePassDependency.srcStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    prePassDependency.srcAccessMask = 0;
    prePassDependency.dstStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    prePassDependency.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo prePassInfo{};
    prePassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    prePassInfo.attachmentCount = static_cast<uint32_t>(prePassAttachments.size());
    prePassInfo.pAttachments = prePassAttachments.data();
    prePassInfo.subpassCount = 1;
    prePassInfo.pSubpasses = &prePassSubpass;
    prePassInfo.dependencyCount = 1;
    prePassInfo.pDependencies = &prePassDependency;
    if (vkCreateRenderPass(device, &prePassInfo, nullptr, &prePass) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create pre-pass render pass!");
    }

    std::array<VkAttachmentDescription, 3> renderPassAttachments = {
        { {
            .format = VK_FORMAT_R16G16B16A16_SFLOAT,// HDR format
            .samples = (VkSampleCountFlagBits)MSAA_SAMPLE_COUNT,
            .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
            .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
          },
          {
            .format = VK_FORMAT_D32_SFLOAT,
            .samples = (VkSampleCountFlagBits)MSAA_SAMPLE_COUNT,
            .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
            .storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
            .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
            .finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
          },
          {
            .format = VK_FORMAT_R16G16B16A16_SFLOAT,// HDR format
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
            .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
          } }
    };

    VkAttachmentReference renderPassColorAttachmentRef = {};
    renderPassColorAttachmentRef.attachment = 0;// index in attachment descriptions array, also index in shader layout
    renderPassColorAttachmentRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkAttachmentReference renderPassDepthAttachmentRef{};
    renderPassDepthAttachmentRef.attachment = 1;
    renderPassDepthAttachmentRef.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkAttachmentReference renderPassColorAttachmentResolveRef = {};
    renderPassColorAttachmentResolveRef.attachment = 2;
    renderPassColorAttachmentResolveRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkSubpassDescription renderPassSubpass = {};
    renderPassSubpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    renderPassSubpass.colorAttachmentCount = 1;
    renderPassSubpass.pColorAttachments = &renderPassColorAttachmentRef;
    renderPassSubpass.pDepthStencilAttachment = &renderPassDepthAttachmentRef;
    renderPassSubpass.pResolveAttachments = &renderPassColorAttachmentResolveRef;

    VkSubpassDependency renderPassDependency = {};
    renderPassDependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    renderPassDependency.dstSubpass = 0;
    renderPassDependency.srcStageMask =
      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    renderPassDependency.srcAccessMask = 0;
    renderPassDependency.dstStageMask =
      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    renderPassDependency.dstAccessMask =
      VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    renderPassInfo.attachmentCount = static_cast<uint32_t>(renderPassAttachments.size());
    renderPassInfo.pAttachments = renderPassAttachments.data();
    renderPassInfo.subpassCount = 1;
    renderPassInfo.pSubpasses = &renderPassSubpass;
    renderPassInfo.dependencyCount = 1;
    renderPassInfo.pDependencies = &renderPassDependency;
    if (vkCreateRenderPass(device, &renderPassInfo, nullptr, &renderPass) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create render pass!");
    }

    std::array<VkAttachmentDescription, 1> postPassAttachments = { { {
      .format = swapchainImageFormat,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
      .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
      .finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
    } } };

    VkAttachmentReference postPassColorAttachmentRef = {};
    postPassColorAttachmentRef.attachment = 0;// index in attachment descriptions array, also index in shader layout
    postPassColorAttachmentRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkSubpassDescription postPassSubpass = {};
    postPassSubpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    postPassSubpass.colorAttachmentCount = 1;
    postPassSubpass.pColorAttachments = &postPassColorAttachmentRef;

    VkSubpassDependency postPassDependency = {};
    postPassDependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    postPassDependency.dstSubpass = 0;
    postPassDependency.srcStageMask =
      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    postPassDependency.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
    postPassDependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    postPassDependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo postPassInfo{};
    postPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    postPassInfo.attachmentCount = static_cast<uint32_t>(postPassAttachments.size());
    postPassInfo.pAttachments = postPassAttachments.data();
    postPassInfo.subpassCount = 1;
    postPassInfo.pSubpasses = &postPassSubpass;
    postPassInfo.dependencyCount = 1;
    postPassInfo.pDependencies = &postPassDependency;
    if (vkCreateRenderPass(device, &postPassInfo, nullptr, &postProcessPass) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create post-pass render pass!");
    }

    // Create framebuffers
    prePassFramebuffers.resize(MAX_FRAMES_IN_FLIGHT);
    for (auto& framebuffer : prePassFramebuffers) {
        std::array<VkImageView, 1> attachments = { getRenderTargetView(msaaDepthImage) };
        VkFramebufferCreateInfo framebufferInfo = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
        framebufferInfo.renderPass = prePass;
        framebufferInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
        framebufferInfo.pAttachments = attachments.data();
        framebufferInfo.width = swapchainExtent.width;
        framebufferInfo.height = swapchainExtent.height;
        framebufferInfo.layers = 1;
        if (vkCreateFramebuffer(device, &framebufferInfo, nullptr, &framebuffer) != VK_SUCCESS) {
            throw std::runtime_error("Failed to create pre-pass framebuffer!");
        }
    }

    renderFramebuffers.resize(MAX_FRAMES_IN_FLIGHT);
    for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        std::array<VkImageView, 3> attachments = { getRenderTargetView(msaaColorImage),
                                                   getRenderTargetView(msaaDepthImage),
                                                   getRenderTargetView(resolveColorImage) };
        VkFramebufferCreateInfo framebufferInfo = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
        framebufferInfo.renderPass = renderPass;
        framebufferInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
        framebufferInfo.pAttachments = attachments.data();
        framebufferInfo.width = swapchainExtent.width;
        framebufferInfo.height = swapchainExtent.height;
        framebufferInfo.layers = 1;
        if (vkCreateFramebuffer(device, &framebufferInfo, nullptr, &renderFramebuffers[i]) != VK_SUCCESS) {
            throw std::runtime_error("Failed to create render framebuffer!");
        }
    }

    postProcessFramebuffers.resize(MAX_FRAMES_IN_FLIGHT);
    for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        std::array<VkImageView, 1> attachments = { swapchainImageViews[i] };
        VkFramebufferCreateInfo framebufferInfo = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
        framebufferInfo.renderPass = postProcessPass;
        framebufferInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
        framebufferInfo.pAttachments = attachments.data();
        framebufferInfo.width = swapchainExtent.width;
        framebufferInfo.height = swapchainExtent.height;
        framebufferInfo.layers = 1;
        if (vkCreateFramebuffer(device, &framebufferInfo, nullptr, &postProcessFramebuffers[i]) != VK_SUCCESS) {
            throw std::runtime_error("Failed to create post-processing framebuffer!");
        }
    }
#endif

    // Create descriptor set layouts
    VkDescriptorSetLayoutCreateInfo emptySetLayoutDesc = {};
    emptySetLayoutDesc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    emptySetLayoutDesc.bindingCount = 0;
    emptySetLayoutDesc.pBindings = nullptr;
    if (vkCreateDescriptorSetLayout(device, &emptySetLayoutDesc, nullptr, &emptySetLayout) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create empty set layout!");
    }

    std::array<VkDescriptorSetLayoutBinding, 6> set0LayoutBindings = { {
      // Camera data
      { .binding = 0,
        .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
        .descriptorCount = 1,
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_COMPUTE_BIT,
        .pImmutableSamplers = nullptr },
      // Instances data
      { .binding = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
        .descriptorCount = 1,
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
        .pImmutableSamplers = nullptr },
      // Directional lights
      { .binding = 2,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .descriptorCount = 1,
        .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
        .pImmutableSamplers = nullptr },
      // Point lights
      { .binding = 3,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .descriptorCount = 1,
        .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT,
        .pImmutableSamplers = nullptr },
      // Light cull data
      { .binding = 4,
        .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
        .descriptorCount = 1,
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
        .pImmutableSamplers = nullptr },
      // Cluster data
      { .binding = 5,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .descriptorCount = 1,
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
        .pImmutableSamplers = nullptr },
    } };
    VkDescriptorSetLayoutCreateInfo set0LayoutDesc = {};
    set0LayoutDesc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    set0LayoutDesc.bindingCount = static_cast<uint32_t>(set0LayoutBindings.size());
    set0LayoutDesc.pBindings = set0LayoutBindings.data();
    if (vkCreateDescriptorSetLayout(device, &set0LayoutDesc, nullptr, &set0Layout) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create set 0 layout!");
    }

    std::array<VkDescriptorSetLayoutBinding, 5> set1LayoutBindings = {
        { // Base map
          { .binding = 0,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
            .pImmutableSamplers = nullptr },
          // Normal map
          { .binding = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
            .pImmutableSamplers = nullptr },
          // Metallic roughness map
          { .binding = 2,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
            .pImmutableSamplers = nullptr },
          // Occlusion map
          { .binding = 3,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
            .pImmutableSamplers = nullptr },
          // Emissive map
          { .binding = 4,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
            .pImmutableSamplers = nullptr } }
    };
    VkDescriptorSetLayoutCreateInfo set1LayoutDesc = {};
    set1LayoutDesc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    set1LayoutDesc.bindingCount = static_cast<uint32_t>(set1LayoutBindings.size());
    set1LayoutDesc.pBindings = set1LayoutBindings.data();
    if (vkCreateDescriptorSetLayout(device, &set1LayoutDesc, nullptr, &set1Layout) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create set 1 layout!");
    }

    std::array<VkDescriptorSetLayoutBinding, 1> set2LayoutBindings = { { { .binding = 0,
                                                                           .descriptorType =
                                                                             VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                                                           .descriptorCount = 1,
                                                                           .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
                                                                           .pImmutableSamplers = nullptr } } };
    VkDescriptorSetLayoutCreateInfo set2LayoutDesc = {};
    set2LayoutDesc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    set2LayoutDesc.bindingCount = static_cast<uint32_t>(set2LayoutBindings.size());
    set2LayoutDesc.pBindings = set2LayoutBindings.data();
    if (vkCreateDescriptorSetLayout(device, &set2LayoutDesc, nullptr, &set2Layout) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create set 2 layout!");
    }

    // Create pipeline layouts
    std::array<VkDescriptorSetLayout, 2> renderDescriptorSetLayouts = { set0Layout, set1Layout };
    std::array<VkPushConstantRange, 2> renderPushConstantRanges = {
        { { VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(glm::vec3) },
          { VK_SHADER_STAGE_VERTEX_BIT, sizeof(glm::vec3), sizeof(Uint32) } }
    };
    VkPipelineLayoutCreateInfo renderPipelineLayoutInfo = {};
    renderPipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    renderPipelineLayoutInfo.setLayoutCount = static_cast<uint32_t>(renderDescriptorSetLayouts.size());
    renderPipelineLayoutInfo.pSetLayouts = renderDescriptorSetLayouts.data();
    renderPipelineLayoutInfo.pushConstantRangeCount = renderPushConstantRanges.size();
    renderPipelineLayoutInfo.pPushConstantRanges = renderPushConstantRanges.data();
    if (vkCreatePipelineLayout(device, &renderPipelineLayoutInfo, nullptr, &renderPipelineLayout) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create render pipeline layout!");
    }

    std::array<VkPushConstantRange, 1> depthOnlyPushConstantRanges = {
        { { VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(Uint32) } }
    };
    std::array<VkDescriptorSetLayout, 2> depthOnlyDescriptorSetLayouts = { set0Layout, set1Layout };
    VkPipelineLayoutCreateInfo depthOnlyPipelineLayoutInfo = {};
    depthOnlyPipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    depthOnlyPipelineLayoutInfo.setLayoutCount = static_cast<uint32_t>(depthOnlyDescriptorSetLayouts.size());
    depthOnlyPipelineLayoutInfo.pSetLayouts = depthOnlyDescriptorSetLayouts.data();
    depthOnlyPipelineLayoutInfo.pushConstantRangeCount = depthOnlyPushConstantRanges.size();
    depthOnlyPipelineLayoutInfo.pPushConstantRanges = depthOnlyPushConstantRanges.data();
    if (vkCreatePipelineLayout(device, &depthOnlyPipelineLayoutInfo, nullptr, &prePassPipelineLayout) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create depth-only pipeline layout!");
    }

    std::array<VkDescriptorSetLayout, 3> postProcessDescriptorSetLayouts = { emptySetLayout,
                                                                             emptySetLayout,
                                                                             set2Layout };
    VkPipelineLayoutCreateInfo postProcessPipelineLayoutInfo = {};
    postProcessPipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    postProcessPipelineLayoutInfo.setLayoutCount = static_cast<uint32_t>(postProcessDescriptorSetLayouts.size());
    postProcessPipelineLayoutInfo.pSetLayouts = postProcessDescriptorSetLayouts.data();
    postProcessPipelineLayoutInfo.pushConstantRangeCount = 0;
    postProcessPipelineLayoutInfo.pPushConstantRanges = nullptr;
    if (vkCreatePipelineLayout(device, &postProcessPipelineLayoutInfo, nullptr, &postProcessPipelineLayout)
        != VK_SUCCESS) {
        throw std::runtime_error("Failed to create post-process pipeline layout!");
    }

    VkPipelineLayoutCreateInfo tileCullingPipelineLayoutInfo = {};
    tileCullingPipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    tileCullingPipelineLayoutInfo.setLayoutCount = 1;
    tileCullingPipelineLayoutInfo.pSetLayouts = &set0Layout;
    tileCullingPipelineLayoutInfo.pushConstantRangeCount = 0;
    tileCullingPipelineLayoutInfo.pPushConstantRanges = nullptr;
    if (vkCreatePipelineLayout(device, &tileCullingPipelineLayoutInfo, nullptr, &tileCullingPipelineLayout)
        != VK_SUCCESS) {
        throw std::runtime_error("Failed to create tile culling pipeline layout!");
    }

    // Create pipelines
    renderPipeline = createRenderPipeline(
      std::string("assets/shaders/TBN.vert.spv"), std::string("assets/shaders/PBRNormalMapped.frag.spv")
    );
    prePassPipeline = createPrePassPipeline(
      std::string("assets/shaders/PrePass.vert.spv"), std::string("assets/shaders/PrePass.frag.spv")
    );
    postProcessPipeline = createPostProcessPipeline(
      std::string("assets/shaders/FullScreen.vert.spv"), std::string("assets/shaders/PostProcess.frag.spv")
    );
    tileCullingPipeline =
      createComputePipeline(std::string("assets/shaders/TileLightCull.comp.spv"), tileCullingPipelineLayout);

    // Create uniform buffers
    cameraDataBuffers.resize(MAX_FRAMES_IN_FLIGHT);
    cameraDataBuffersMapped.resize(MAX_FRAMES_IN_FLIGHT);
    for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        cameraDataBuffers[i] =
          createBufferMapped(BufferUsage::UNIFORM, sizeof(CameraData), &cameraDataBuffersMapped[i]);
    }
    instanceDataBuffers.resize(MAX_FRAMES_IN_FLIGHT);
    instanceDataBuffersMapped.resize(MAX_FRAMES_IN_FLIGHT);
    for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        instanceDataBuffers[i] =
          createBufferMapped(BufferUsage::UNIFORM, sizeof(InstanceData) * MAX_INSTANCES, &instanceDataBuffersMapped[i]);
    }

    // Create textures
    defaultAlbedoTexture = createTexture(AssetManager::loadImage("assets/textures/default_albedo.png"));
    defaultNormalTexture = createTexture(AssetManager::loadImage("assets/textures/default_norm.png"));
    defaultORMTexture = createTexture(AssetManager::loadImage("assets/textures/default_orm.png"));
    defaultEmissiveTexture = createTexture(AssetManager::loadImage("assets/textures/default_emissive.png"));
    // defaultDisplacementTexture = createTexture(AssetManager::loadImage("assets/textures/default_disp.png"));

    // Create samplers
    VkSamplerCreateInfo samplerInfo = {};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    VkPhysicalDeviceProperties properties = {};
    vkGetPhysicalDeviceProperties(physicalDevice, &properties);

    float maxAnisotropy = properties.limits.maxSamplerAnisotropy;
    if (maxAnisotropy > 1.0) {
        samplerInfo.maxAnisotropy = maxAnisotropy;
        samplerInfo.anisotropyEnable = VK_TRUE;
    } else {
        samplerInfo.maxAnisotropy = 1.0;
        samplerInfo.anisotropyEnable = VK_FALSE;
    }
    samplerInfo.compareEnable = VK_FALSE;
    samplerInfo.compareOp = VK_COMPARE_OP_NEVER;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    samplerInfo.mipLodBias = 0.0f;
    samplerInfo.minLod = 0.0f;
    samplerInfo.maxLod = VK_LOD_CLAMP_NONE;// NOTE: this value is for clamping, and 15.0f might be enough
    samplerInfo.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    if (vkCreateSampler(device, &samplerInfo, nullptr, &defaultSampler) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create texture sampler!");
    }

    // Create descriptor pool and sets
    std::array<VkDescriptorPoolSize, 2> set0PoolSizes = {
        { { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 3 * static_cast<uint32_t>(MAX_FRAMES_IN_FLIGHT) },
          { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3 * static_cast<uint32_t>(MAX_FRAMES_IN_FLIGHT) } }
    };

    VkDescriptorPoolCreateInfo set0PoolDesc{};
    set0PoolDesc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    set0PoolDesc.poolSizeCount = set0PoolSizes.size();
    set0PoolDesc.pPoolSizes = set0PoolSizes.data();
    set0PoolDesc.maxSets = static_cast<uint32_t>(MAX_FRAMES_IN_FLIGHT);
    if (vkCreateDescriptorPool(device, &set0PoolDesc, nullptr, &set0DescriptorPool) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create frame descriptor pool!");
    }

    std::vector<VkDescriptorSetLayout> set0Layouts(MAX_FRAMES_IN_FLIGHT, set0Layout);
    VkDescriptorSetAllocateInfo set0AllocInfo = {};
    set0AllocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    set0AllocInfo.descriptorPool = set0DescriptorPool;
    set0AllocInfo.descriptorSetCount = static_cast<uint32_t>(MAX_FRAMES_IN_FLIGHT);
    set0AllocInfo.pSetLayouts = set0Layouts.data();

    set0s.resize(MAX_FRAMES_IN_FLIGHT);
    if (vkAllocateDescriptorSets(device, &set0AllocInfo, set0s.data()) != VK_SUCCESS) {
        throw std::runtime_error("Failed to allocate set 0 descriptor sets!");
    }

    std::array<VkDescriptorPoolSize, 1> set2PoolSizes = { {
      { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1 * static_cast<uint32_t>(MAX_FRAMES_IN_FLIGHT) },
    } };

    VkDescriptorPoolCreateInfo set2PoolDesc{};
    set2PoolDesc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    set2PoolDesc.poolSizeCount = set2PoolSizes.size();
    set2PoolDesc.pPoolSizes = set2PoolSizes.data();
    set2PoolDesc.maxSets = static_cast<uint32_t>(MAX_FRAMES_IN_FLIGHT);
    if (vkCreateDescriptorPool(device, &set2PoolDesc, nullptr, &set2DescriptorPool) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create set 2 descriptor pool!");
    }

    std::vector<VkDescriptorSetLayout> set2Layouts(MAX_FRAMES_IN_FLIGHT, set2Layout);
    VkDescriptorSetAllocateInfo set2AllocInfo = {};
    set2AllocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    set2AllocInfo.descriptorPool = set2DescriptorPool;
    set2AllocInfo.descriptorSetCount = static_cast<uint32_t>(MAX_FRAMES_IN_FLIGHT);
    set2AllocInfo.pSetLayouts = set2Layouts.data();

    set2s.resize(MAX_FRAMES_IN_FLIGHT);
    if (vkAllocateDescriptorSets(device, &set2AllocInfo, set2s.data()) != VK_SUCCESS) {
        throw std::runtime_error("Failed to allocate set 2 descriptor sets!");
    }

    // Write descriptor sets
    for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        VkDescriptorBufferInfo cameraDataBufferInfo = {};
        cameraDataBufferInfo.buffer = getBuffer(cameraDataBuffers[i]);
        cameraDataBufferInfo.offset = 0;
        cameraDataBufferInfo.range = sizeof(CameraData);

        VkDescriptorBufferInfo instanceBufferInfo = {};
        instanceBufferInfo.buffer = getBuffer(instanceDataBuffers[i]);
        instanceBufferInfo.offset = 0;
        instanceBufferInfo.range = sizeof(InstanceData) * MAX_INSTANCES;

        std::array<VkWriteDescriptorSet, 2> writes = { { // must match descriptor set layout bindings
                                                         {
                                                           .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                                           .dstSet = set0s[i],
                                                           .dstBinding = 0,
                                                           .dstArrayElement = 0,
                                                           .descriptorCount = 1,
                                                           .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                                                           .pImageInfo = nullptr,
                                                           .pBufferInfo = &cameraDataBufferInfo,
                                                         },
                                                         {
                                                           .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                                           .dstSet = set0s[i],
                                                           .dstBinding = 1,
                                                           .dstArrayElement = 0,
                                                           .descriptorCount = 1,
                                                           .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                                                           .pImageInfo = nullptr,
                                                           .pBufferInfo = &instanceBufferInfo,
                                                         } } };
        vkUpdateDescriptorSets(device, writes.size(), writes.data(), 0, nullptr);
    }
    for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        VkDescriptorImageInfo colorImageInfo = {};
        colorImageInfo.imageView = getRenderTargetView(resolveColorImage);
        colorImageInfo.sampler = defaultSampler;
        colorImageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkWriteDescriptorSet write = {
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = set2s[i],
            .dstBinding = 0,
            .dstArrayElement = 0,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .pImageInfo = &colorImageInfo,
            .pBufferInfo = nullptr,
        };
        vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
    }
}

auto Renderer_Vulkan::stage(std::shared_ptr<Scene> scene) -> void {
    ZoneScoped;

    // Lights
    directionalLightBuffers.resize(MAX_FRAMES_IN_FLIGHT);
    directionalLightBuffersMapped.resize(MAX_FRAMES_IN_FLIGHT);
    for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        directionalLightBuffers[i] = createBufferMapped(
          BufferUsage::STORAGE,
          sizeof(DirectionalLight) * scene->directionalLights.size(),
          &directionalLightBuffersMapped[i]
        );
        memcpy(
          directionalLightBuffersMapped[i],
          scene->directionalLights.data(),
          sizeof(DirectionalLight) * scene->directionalLights.size()
        );
    }

    pointLightBuffers.resize(MAX_FRAMES_IN_FLIGHT);
    pointLightBuffersMapped.resize(MAX_FRAMES_IN_FLIGHT);
    for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        pointLightBuffers[i] = createBufferMapped(
          BufferUsage::STORAGE, sizeof(PointLight) * scene->pointLights.size(), &pointLightBuffersMapped[i]
        );
        memcpy(pointLightBuffersMapped[i], scene->pointLights.data(), sizeof(PointLight) * scene->pointLights.size());
    }

    LightCullData lightCullData = {};
    lightCullData.screenSize = glm::vec2(swapchainExtent.width, swapchainExtent.height);
    lightCullData.gridSize = glm::uvec3(clusterGridSizeX, clusterGridSizeY, clusterGridSizeZ);
    lightCullData.lightCount = scene->pointLights.size();
    lightCullDataBuffers.resize(MAX_FRAMES_IN_FLIGHT);
    for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        lightCullDataBuffers[i] = createBuffer(BufferUsage::UNIFORM, sizeof(LightCullData));
        VkDeviceMemory bufferMemory = getBufferMemory(lightCullDataBuffers[i]);
        void* data;
        vkMapMemory(device, bufferMemory, 0, sizeof(LightCullData), 0, &data);
        memcpy(data, &lightCullData, sizeof(LightCullData));
        vkUnmapMemory(device, bufferMemory);
    }

    clusterBuffers.resize(MAX_FRAMES_IN_FLIGHT);
    for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        clusterBuffers[i] =
          createBuffer(BufferUsage::STORAGE, sizeof(Cluster) * clusterGridSizeX * clusterGridSizeY * clusterGridSizeZ);
    }

    // Textures
    for (auto& img : scene->images) {
        img->texture = createTexture(img);
    }

    // Pipelines
    if (scene->materials.empty()) {
        // TODO: create default material
    }
    for (auto& mat : scene->materials) {
        // pipelines[mat->pipeline] = createPipeline();
        materialIDs[mat] = nextMaterialID++;
    }

    // Buffers
    const std::function<void(const std::shared_ptr<Node>&)> stageNode = [&](const std::shared_ptr<Node>& node) {
        if (node->meshGroup) {
            for (auto& mesh : node->meshGroup->meshes) {
                mesh->vbos.push_back(createVertexBuffer(mesh->vertices));// TODO: use single vbo for all meshes
                mesh->ebo = createIndexBuffer(mesh->indices);
                mesh->materialID = materialIDs.at(mesh->material);
                mesh->instanceID = nextInstanceID++;
            }
        }
        for (const auto& child : node->children) {
            stageNode(child);
        }
    };
    for (auto& node : scene->nodes) {
        stageNode(node);
    }

    // Descriptor sets
    std::array<VkDescriptorPoolSize, 1> set1PoolSizes = { { { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                                              5 * static_cast<uint32_t>(scene->materials.size()) } } };

    VkDescriptorPoolCreateInfo set1PoolDesc{};
    set1PoolDesc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    set1PoolDesc.poolSizeCount = set1PoolSizes.size();
    set1PoolDesc.pPoolSizes = set1PoolSizes.data();
    set1PoolDesc.maxSets = static_cast<uint32_t>(scene->materials.size());
    if (vkCreateDescriptorPool(device, &set1PoolDesc, nullptr, &set1DescriptorPool) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create material descriptor pool!");
    }

    std::vector<VkDescriptorSetLayout> set1Layouts(scene->materials.size(), set1Layout);
    VkDescriptorSetAllocateInfo set1AllocInfo = {};
    set1AllocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    set1AllocInfo.descriptorPool = set1DescriptorPool;
    set1AllocInfo.descriptorSetCount = static_cast<uint32_t>(scene->materials.size());
    set1AllocInfo.pSetLayouts = set1Layouts.data();

    set1s.resize(scene->materials.size());
    if (vkAllocateDescriptorSets(device, &set1AllocInfo, set1s.data()) != VK_SUCCESS) {
        throw std::runtime_error("Failed to allocate material descriptor sets!");
    }

    for (size_t i = 0; i < scene->materials.size(); i++) {
        auto& mat = scene->materials[i];

        VkDescriptorImageInfo albedoImageInfo;
        albedoImageInfo.imageView = getTextureView(mat->albedoMap ? mat->albedoMap->texture : defaultAlbedoTexture);
        albedoImageInfo.sampler = defaultSampler;
        albedoImageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkDescriptorImageInfo normalImageInfo;
        normalImageInfo.imageView = getTextureView(mat->normalMap ? mat->normalMap->texture : defaultNormalTexture);
        normalImageInfo.sampler = defaultSampler;
        normalImageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkDescriptorImageInfo metallicRoughnessImageInfo;
        metallicRoughnessImageInfo.imageView =
          getTextureView(mat->roughnessMap ? mat->roughnessMap->texture : defaultORMTexture);
        metallicRoughnessImageInfo.sampler = defaultSampler;
        metallicRoughnessImageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkDescriptorImageInfo occlusionImageInfo;
        occlusionImageInfo.imageView =
          getTextureView(mat->occlusionMap ? mat->occlusionMap->texture : defaultORMTexture);
        occlusionImageInfo.sampler = defaultSampler;
        occlusionImageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkDescriptorImageInfo emissiveImageInfo;
        emissiveImageInfo.imageView =
          getTextureView(mat->emissiveMap ? mat->emissiveMap->texture : defaultEmissiveTexture);
        emissiveImageInfo.sampler = defaultSampler;
        emissiveImageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        std::array<VkWriteDescriptorSet, 5> writes = { { // must match descriptor set layout bindings
                                                         {
                                                           .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                                           .dstSet = set1s[i],
                                                           .dstBinding = 0,
                                                           .dstArrayElement = 0,
                                                           .descriptorCount = 1,
                                                           .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                                           .pImageInfo = &albedoImageInfo,
                                                           .pBufferInfo = nullptr,
                                                         },
                                                         {
                                                           .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                                           .dstSet = set1s[i],
                                                           .dstBinding = 1,
                                                           .dstArrayElement = 0,
                                                           .descriptorCount = 1,
                                                           .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                                           .pImageInfo = &normalImageInfo,
                                                           .pBufferInfo = nullptr,
                                                         },
                                                         {
                                                           .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                                           .dstSet = set1s[i],
                                                           .dstBinding = 2,
                                                           .dstArrayElement = 0,
                                                           .descriptorCount = 1,
                                                           .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                                           .pImageInfo = &metallicRoughnessImageInfo,
                                                           .pBufferInfo = nullptr,
                                                         },
                                                         {
                                                           .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                                           .dstSet = set1s[i],
                                                           .dstBinding = 3,
                                                           .dstArrayElement = 0,
                                                           .descriptorCount = 1,
                                                           .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                                           .pImageInfo = &occlusionImageInfo,
                                                           .pBufferInfo = nullptr,
                                                         },
                                                         {
                                                           .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                                           .dstSet = set1s[i],
                                                           .dstBinding = 4,
                                                           .dstArrayElement = 0,
                                                           .descriptorCount = 1,
                                                           .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                                           .pImageInfo = &emissiveImageInfo,
                                                           .pBufferInfo = nullptr,
                                                         } } };
        vkUpdateDescriptorSets(device, writes.size(), writes.data(), 0, nullptr);

        materialTextureSets[mat] = set1s[i];
    }

    for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        VkDescriptorBufferInfo directionalLightBufferInfo = {};
        directionalLightBufferInfo.buffer = getBuffer(directionalLightBuffers[i]);
        directionalLightBufferInfo.offset = 0;
        directionalLightBufferInfo.range = sizeof(DirectionalLight) * scene->directionalLights.size();

        VkDescriptorBufferInfo pointLightBufferInfo = {};
        pointLightBufferInfo.buffer = getBuffer(pointLightBuffers[i]);
        pointLightBufferInfo.offset = 0;
        pointLightBufferInfo.range = sizeof(PointLight) * scene->pointLights.size();

        VkDescriptorBufferInfo lightCullDataBufferInfo = {};
        lightCullDataBufferInfo.buffer = getBuffer(lightCullDataBuffers[i]);
        lightCullDataBufferInfo.offset = 0;
        lightCullDataBufferInfo.range = sizeof(LightCullData);

        VkDescriptorBufferInfo clusterBufferInfo = {};
        clusterBufferInfo.buffer = getBuffer(clusterBuffers[i]);
        clusterBufferInfo.offset = 0;
        clusterBufferInfo.range = sizeof(Cluster) * clusterGridSizeX * clusterGridSizeY * clusterGridSizeZ;

        std::array<VkWriteDescriptorSet, 4> writes = { { // must match descriptor set layout bindings
                                                         {
                                                           .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                                           .dstSet = set0s[i],
                                                           .dstBinding = 2,
                                                           .dstArrayElement = 0,
                                                           .descriptorCount = 1,
                                                           .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                                           .pImageInfo = nullptr,
                                                           .pBufferInfo = &directionalLightBufferInfo,
                                                         },
                                                         {
                                                           .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                                           .dstSet = set0s[i],
                                                           .dstBinding = 3,
                                                           .dstArrayElement = 0,
                                                           .descriptorCount = 1,
                                                           .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                                           .pImageInfo = nullptr,
                                                           .pBufferInfo = &pointLightBufferInfo,
                                                         },
                                                         {
                                                           .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                                           .dstSet = set0s[i],
                                                           .dstBinding = 4,
                                                           .dstArrayElement = 0,
                                                           .descriptorCount = 1,
                                                           .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                                                           .pImageInfo = nullptr,
                                                           .pBufferInfo = &lightCullDataBufferInfo,
                                                         },
                                                         {
                                                           .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                                           .dstSet = set0s[i],
                                                           .dstBinding = 5,
                                                           .dstArrayElement = 0,
                                                           .descriptorCount = 1,
                                                           .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                                           .pImageInfo = nullptr,
                                                           .pBufferInfo = &clusterBufferInfo,
                                                         } } };
        vkUpdateDescriptorSets(device, writes.size(), writes.data(), 0, nullptr);
    }

    // Initialize particle system
    initParticleSystem();
}

auto Renderer_Vulkan::draw(std::shared_ptr<Scene> scene, Camera& camera) -> void {
    ZoneScoped;
    FrameMark;

    vkWaitForFences(device, 1, &renderFences[currentFrameInFlight], VK_TRUE, UINT64_MAX);
    vkResetFences(device, 1, &renderFences[currentFrameInFlight]);

    uint32_t swapchainImageIndex;
    if (vkAcquireNextImageKHR(
          device,
          swapchain,
          UINT64_MAX,
          imageAvailableSemaphores[currentFrameInFlight],
          VK_NULL_HANDLE,
          &swapchainImageIndex
        )
        != VK_SUCCESS) {
        return;
    }

    // Prepare data
    float time = SDL_GetTicks() / 1000.0;

    glm::vec3 camPos = camera.getEye();
    glm::mat4 view = camera.getViewMatrix();
    glm::mat4 proj = camera.getProjMatrix();
    glm::mat4 invProj = glm::inverse(proj);
    glm::mat4 invView = glm::inverse(view);
    CameraData cameraData = {
        .proj = proj,
        .view = view,
        .invProj = invProj,
        .invView = invView,
        .near = camera.near(),
        .far = camera.far(),
        .position = camPos,
    };
    memcpy(cameraDataBuffersMapped[currentFrameInFlight], &cameraData, sizeof(CameraData));
    memcpy(
      directionalLightBuffersMapped[currentFrameInFlight],
      scene->directionalLights.data(),
      sizeof(DirectionalLight) * scene->directionalLights.size()
    );
    memcpy(
      pointLightBuffersMapped[currentFrameInFlight],
      scene->pointLights.data(),
      sizeof(PointLight) * scene->pointLights.size()
    );

    instances.clear();
    const std::function<void(const std::shared_ptr<Node>&)> updateNode = [&](const std::shared_ptr<Node>& node) {
        if (node->meshGroup) {
            const glm::mat4& transform = node->worldTransform;
            for (auto& mesh : node->meshGroup->meshes) {
                instances.push_back({ .model = transform,
                                      .vertexOffset = mesh->vertexOffset,
                                      .indexOffset = mesh->indexOffset,
                                      .vertexCount = mesh->vertexCount,
                                      .indexCount = mesh->indexCount,
                                      .materialID = mesh->materialID,
                                      .primitiveMode = mesh->primitiveMode,
                                      .AABBMin = mesh->worldAABBMin,
                                      .AABBMax = mesh->worldAABBMax });
            }
        }
        for (const auto& child : node->children) {
            updateNode(child);
        }
    };
    for (const auto& node : scene->nodes) {
        updateNode(node);
    }
    if (instances.size() > MAX_INSTANCES) {// TODO: reallocate when needed
        fmt::print("Warning: Instance count ({}) exceeds MAX_INSTANCES ({})\n", instances.size(), MAX_INSTANCES);
    }
    memcpy(instanceDataBuffersMapped[currentFrameInFlight], instances.data(), sizeof(InstanceData) * instances.size());

    VkCommandBuffer cmd = commandBuffers[currentFrameInFlight];
    vkResetCommandBuffer(cmd, 0);
    VkCommandBufferBeginInfo beginInfo = {};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = 0;
    beginInfo.pInheritanceInfo = nullptr;// only for secondary command buffer
    if (vkBeginCommandBuffer(cmd, &beginInfo) != VK_SUCCESS) {
        throw std::runtime_error("Failed to begin recording command buffer!");
    }

#if !(USE_DYNAMIC_RENDERING)
    VkRenderPassBeginInfo prePassInfo{};
    prePassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    prePassInfo.renderPass = prePass;
    prePassInfo.framebuffer = prePassFramebuffers[swapchainImageIndex];
    prePassInfo.renderArea = { 0, 0, swapchainExtent.width, swapchainExtent.height };
    std::array<VkClearValue, 1> prePassClearValues;
    prePassClearValues[0].depthStencil = { static_cast<float>(clearDepth), 0 };
    prePassInfo.clearValueCount = prePassClearValues.size();
    prePassInfo.pClearValues = prePassClearValues.data();

    VkRenderPassBeginInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    renderPassInfo.renderPass = renderPass;
    renderPassInfo.framebuffer = renderFramebuffers[swapchainImageIndex];
    renderPassInfo.renderArea = { 0, 0, swapchainExtent.width, swapchainExtent.height };
    std::array<VkClearValue, 3> renderPassClearValues;
    renderPassClearValues[0].color = { clearColor.r, clearColor.g, clearColor.b, clearColor.a };
    renderPassClearValues[1].depthStencil = { static_cast<float>(clearDepth), 0 };
    renderPassClearValues[2].color = { clearColor.r, clearColor.g, clearColor.b, clearColor.a };
    renderPassInfo.clearValueCount = renderPassClearValues.size();
    renderPassInfo.pClearValues = renderPassClearValues.data();

    VkRenderPassBeginInfo postProcessPassInfo{};
    postProcessPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    postProcessPassInfo.renderPass = postProcessPass;
    postProcessPassInfo.framebuffer = postProcessFramebuffers[swapchainImageIndex];
    postProcessPassInfo.renderArea = { 0, 0, swapchainExtent.width, swapchainExtent.height };
    std::array<VkClearValue, 1> postProcessClearValues;
    postProcessClearValues[0].color = { clearColor.r, clearColor.g, clearColor.b, clearColor.a };
    postProcessPassInfo.clearValueCount = postProcessClearValues.size();
    postProcessPassInfo.pClearValues = postProcessClearValues.data();

    vkCmdBeginRenderPass(cmd, &prePassInfo, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, prePassPipeline);

    const std::function<void(const std::shared_ptr<Node>&)> drawNodeDepth = [&](const std::shared_ptr<Node>& node) {
        if (node->meshGroup) {
            for (auto& mesh : node->meshGroup->meshes) {
                if (!mesh->material) {
                    fmt::print("No material found for mesh in mesh group {}\n", node->meshGroup->name);
                    continue;
                }
                // vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, getPipeline(mesh->material->pipeline.rid));
                VkBuffer vertexBuffers[] = { getBuffer(mesh->vbos[0]) };
                VkDeviceSize offsets[] = { 0 };
                vkCmdBindVertexBuffers(cmd, 0, 1, vertexBuffers, offsets);
                vkCmdBindIndexBuffer(cmd, getBuffer(mesh->ebo), 0, VkIndexType::VK_INDEX_TYPE_UINT32);
                // TODO: bind camera and instance data buffer outside the loop
                std::array<VkDescriptorSet, 2> descriptorSets = { set0s[currentFrameInFlight],
                                                                  materialTextureSets.at(mesh->material) };
                vkCmdBindDescriptorSets(
                  cmd,
                  VK_PIPELINE_BIND_POINT_GRAPHICS,
                  prePassPipelineLayout,
                  0,
                  descriptorSets.size(),
                  descriptorSets.data(),
                  0,
                  nullptr
                );// resources are set here
                vkCmdPushConstants(
                  cmd, prePassPipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(Uint32), &mesh->instanceID
                );
                vkCmdDrawIndexed(cmd, mesh->indices.size(), 1, 0, 0, 0);
            }
        }
        for (const auto& child : node->children) {
            drawNodeDepth(child);
        }
    };
    for (auto& node : scene->nodes) {
        drawNodeDepth(node);
    }

    vkCmdEndRenderPass(cmd);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, tileCullingPipeline);
    vkCmdBindDescriptorSets(
      cmd, VK_PIPELINE_BIND_POINT_COMPUTE, tileCullingPipelineLayout, 0, 1, &set0s[currentFrameInFlight], 0, nullptr
    );
    vkCmdDispatch(cmd, clusterGridSizeX, clusterGridSizeY, 1);

    // Particle simulation (compute pass)
    float deltaTime = 1.0f / 60.0f; // TODO: pass actual delta time
    glm::vec3 attractorPos = camPos + glm::normalize(glm::vec3(glm::inverse(view) * glm::vec4(0.0f, 0.0f, -1.0f, 0.0f))) * 3.0f;
    updateParticleSimulation(cmd, deltaTime, attractorPos);

    vkCmdBeginRenderPass(cmd, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, renderPipeline);

    const std::function<void(const std::shared_ptr<Node>&)> drawNode = [&](const std::shared_ptr<Node>& node) {
        if (node->meshGroup) {
            for (auto& mesh : node->meshGroup->meshes) {
                if (!mesh->material) {
                    fmt::print("No material found for mesh in mesh group {}\n", node->meshGroup->name);
                    continue;
                }
                // vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, getPipeline(mesh->material->pipeline.rid));
                VkBuffer vertexBuffers[] = { getBuffer(mesh->vbos[0]) };
                VkDeviceSize offsets[] = { 0 };
                vkCmdBindVertexBuffers(cmd, 0, 1, vertexBuffers, offsets);
                vkCmdBindIndexBuffer(cmd, getBuffer(mesh->ebo), 0, VkIndexType::VK_INDEX_TYPE_UINT32);
                // TODO: bind camera and instance data buffer outside the loop
                std::array<VkDescriptorSet, 2> descriptorSets = { set0s[currentFrameInFlight],
                                                                  materialTextureSets.at(mesh->material) };
                vkCmdBindDescriptorSets(
                  cmd,
                  VK_PIPELINE_BIND_POINT_GRAPHICS,
                  renderPipelineLayout,
                  0,
                  descriptorSets.size(),
                  descriptorSets.data(),
                  0,
                  nullptr
                );// resources are set here
                vkCmdPushConstants(
                  cmd, renderPipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(glm::vec3), &camPos
                );
                vkCmdPushConstants(
                  cmd,
                  renderPipelineLayout,
                  VK_SHADER_STAGE_VERTEX_BIT,
                  sizeof(glm::vec3),
                  sizeof(Uint32),
                  &mesh->instanceID
                );
                vkCmdDrawIndexed(cmd, mesh->indices.size(), 1, 0, 0, 0);
            }
        }
        for (const auto& child : node->children) {
            drawNode(child);
        }
    };
    for (auto& node : scene->nodes) {
        drawNode(node);
    }

    // Render particles (after scene, before post-process)
    renderParticles(cmd);

    vkCmdEndRenderPass(cmd);

    vkCmdBeginRenderPass(cmd, &postProcessPassInfo, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, postProcessPipeline);
    std::array<VkDescriptorSet, 1> descriptorSets = { set2s[currentFrameInFlight] };
    // TODO: research descriptor binding best practices
    vkCmdBindDescriptorSets(
      cmd,
      VK_PIPELINE_BIND_POINT_GRAPHICS,
      postProcessPipelineLayout,
      2,
      descriptorSets.size(),
      descriptorSets.data(),
      0,
      nullptr
    );
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRenderPass(cmd);
#else
    insertImageMemoryBarrier(
      cmd,
      swapchainImages[swapchainImageIndex],
      0,
      VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
      VK_IMAGE_LAYOUT_UNDEFINED,
      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
      { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 }
    );
    // insertImageMemoryBarrier(
    //     cmd, depthImage,
    //     0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
    //     VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
    //     VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
    //     { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1 }
    // );

    VkRenderingAttachmentInfo colorAttachment = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    colorAttachment.imageView = swapchainImageViews[swapchainImageIndex];
    colorAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    colorAttachment.resolveMode = VK_RESOLVE_MODE_AVERAGE_BIT;
    colorAttachment.resolveImageView = colorImageView;
    colorAttachment.resolveImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    colorAttachment.clearValue.color = { 0.0f, 0.0f, 0.2f, 0.0f };

    VkRenderingAttachmentInfo depthAttachment = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    depthAttachment.imageView = depthImageView;
    depthAttachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    depthAttachment.resolveMode = VK_RESOLVE_MODE_NONE;
    depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    depthAttachment.clearValue.depthStencil = { 0.0f, 0 };

    VkRenderingInfo renderingInfo = { VK_STRUCTURE_TYPE_RENDERING_INFO };
    renderingInfo.renderArea = { 0, 0, swapchainExtent.width, swapchainExtent.height };
    renderingInfo.layerCount = 1;
    renderingInfo.colorAttachmentCount = 1;
    renderingInfo.pColorAttachments = &colorAttachment;
    renderingInfo.pDepthAttachment = &depthAttachment;

    vkCmdBeginRendering(cmd, &renderingInfo);

    // vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, testDrawPipeline);
    // vkCmdDraw(cmd, 6, 1, 0, 0);

    vkCmdEndRendering(cmd);

    insertImageMemoryBarrier(
      cmd,
      swapchainImages[swapchainImageIndex],
      VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
      0,
      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
      VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
      VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
      { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 }
    );
#endif

    if (vkEndCommandBuffer(cmd) != VK_SUCCESS) {
        throw std::runtime_error("Failed to end recording command buffer!");
    }

    VkPipelineStageFlags waitStages[] = { VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT };
    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.pWaitDstStageMask = waitStages;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &cmd;
    submitInfo.waitSemaphoreCount = 1;
    submitInfo.pWaitSemaphores = &imageAvailableSemaphores[currentFrameInFlight];
    submitInfo.signalSemaphoreCount = 1;
    submitInfo.pSignalSemaphores = &renderFinishedSemaphores[currentFrameInFlight];
    if (vkQueueSubmit(graphicsQueue, 1, &submitInfo, renderFences[currentFrameInFlight]) != VK_SUCCESS) {
        throw std::runtime_error("Failed to submit draw command buffer!");
    }

    VkPresentInfoKHR presentInfo{};
    presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pWaitSemaphores = &renderFinishedSemaphores[currentFrameInFlight];
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = &swapchain;
    presentInfo.pImageIndices = &swapchainImageIndex;
    presentInfo.pResults = nullptr;
    vkQueuePresentKHR(presentQueue, &presentInfo);

    currentFrameInFlight = (currentFrameInFlight + 1) % MAX_FRAMES_IN_FLIGHT;
}

VkShaderModule Renderer_Vulkan::createShaderModule(const std::string& code) {
    VkShaderModuleCreateInfo shaderModuleInfo = {};
    shaderModuleInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    shaderModuleInfo.codeSize = code.size();
    shaderModuleInfo.pCode = reinterpret_cast<const uint32_t*>(code.data());
    VkShaderModule shaderModule;
    if (vkCreateShaderModule(device, &shaderModuleInfo, nullptr, &shaderModule) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create shader module!");
    }
    return shaderModule;
}

VkPipeline Renderer_Vulkan::createPipeline(const std::string& filename1, const std::string& filename2) {
    // TODO: add a generic pipeline creation function
    return createRenderPipeline(filename1, filename2);
}

VkPipeline Renderer_Vulkan::createRenderPipeline(const std::string& vertShader, const std::string& fragShader) {
    // Shader stages
    auto vertShaderCode = readFile(vertShader);
    auto fragShaderCode = readFile(fragShader);
    auto vertShaderModule = createShaderModule(vertShaderCode);
    auto fragShaderModule = createShaderModule(fragShaderCode);

    VkPipelineShaderStageCreateInfo vertShaderStageInfo{};
    vertShaderStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    vertShaderStageInfo.stage = VK_SHADER_STAGE_VERTEX_BIT;
    vertShaderStageInfo.module = vertShaderModule;
    vertShaderStageInfo.pName = "main";

    VkPipelineShaderStageCreateInfo fragShaderStageInfo{};
    fragShaderStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    fragShaderStageInfo.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    fragShaderStageInfo.module = fragShaderModule;
    fragShaderStageInfo.pName = "main";

    std::array<VkPipelineShaderStageCreateInfo, 2> shaderStages = { vertShaderStageInfo, fragShaderStageInfo };

    // Vertex input state
    std::array<VkVertexInputBindingDescription, 1> vertexBindingDescriptions = {
        { { 0, sizeof(VertexData), VK_VERTEX_INPUT_RATE_VERTEX } }
    };
    std::array<VkVertexInputAttributeDescription, 4> vertexAttributeDescriptions = {
        { { 0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(VertexData, position) },
          { 1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(VertexData, uv) },
          { 2, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(VertexData, normal) },
          { 3, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(VertexData, tangent) } }
    };

    VkPipelineVertexInputStateCreateInfo vertexInputStateInfo = {
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO
    };
    vertexInputStateInfo.vertexBindingDescriptionCount = vertexBindingDescriptions.size();
    vertexInputStateInfo.pVertexBindingDescriptions = vertexBindingDescriptions.data();
    vertexInputStateInfo.vertexAttributeDescriptionCount = vertexAttributeDescriptions.size();
    vertexInputStateInfo.pVertexAttributeDescriptions = vertexAttributeDescriptions.data();

    // Input assembly state
    VkPipelineInputAssemblyStateCreateInfo inputAssemblyStateInfo = {
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO
    };
    inputAssemblyStateInfo.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    // Viewport state
    VkViewport viewport = {};
    viewport.x = 0.0f;
    viewport.y = (float)swapchainExtent.height;
    viewport.width = (float)swapchainExtent.width;
    viewport.height = -(float)swapchainExtent.height;
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    VkRect2D scissor = {};
    scissor.offset = { 0, 0 };
    scissor.extent = swapchainExtent;
    VkPipelineViewportStateCreateInfo viewportStateInfo = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    viewportStateInfo.viewportCount = 1;
    viewportStateInfo.pViewports = &viewport;
    viewportStateInfo.scissorCount = 1;
    viewportStateInfo.pScissors = &scissor;

    // Rasterization state
    VkPipelineRasterizationStateCreateInfo rasterizationStateInfo = {
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO
    };
    rasterizationStateInfo.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizationStateInfo.cullMode = VK_CULL_MODE_BACK_BIT;
    rasterizationStateInfo.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterizationStateInfo.depthClampEnable = VK_FALSE;
    rasterizationStateInfo.rasterizerDiscardEnable = VK_FALSE;
    rasterizationStateInfo.depthBiasEnable = VK_FALSE;
    rasterizationStateInfo.lineWidth = 1.0f;

    // Multisample state
    VkPipelineMultisampleStateCreateInfo multisampleStateInfo = {};
    multisampleStateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampleStateInfo.rasterizationSamples = (VkSampleCountFlagBits)MSAA_SAMPLE_COUNT;

    // Depth and stencil state
    VkPipelineDepthStencilStateCreateInfo depthStencilStateInfo = {};
    depthStencilStateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencilStateInfo.depthTestEnable = true;
    depthStencilStateInfo.depthWriteEnable = false;
    depthStencilStateInfo.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    depthStencilStateInfo.depthBoundsTestEnable = false;
    depthStencilStateInfo.stencilTestEnable = false;

    // Color blend state
    VkPipelineColorBlendAttachmentState blendAttachmentState = {};
    blendAttachmentState.colorWriteMask = 0xf;
    blendAttachmentState.blendEnable = VK_FALSE;
    VkPipelineColorBlendStateCreateInfo colorBlendStateInfo = {
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO
    };
    colorBlendStateInfo.attachmentCount = 1;
    colorBlendStateInfo.pAttachments = &blendAttachmentState;

    // Dynamic states
    std::vector<VkDynamicState> enabledDynamicStates = {};
    VkPipelineDynamicStateCreateInfo dynamicStateInfo = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    dynamicStateInfo.dynamicStateCount = static_cast<uint32_t>(enabledDynamicStates.size());
    dynamicStateInfo.pDynamicStates = enabledDynamicStates.data();

    // Pipeline creation
    VkGraphicsPipelineCreateInfo pipelineInfo = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    pipelineInfo.layout = renderPipelineLayout;
    pipelineInfo.stageCount = static_cast<uint32_t>(shaderStages.size());
    pipelineInfo.pStages = shaderStages.data();
    pipelineInfo.pVertexInputState = &vertexInputStateInfo;
    pipelineInfo.pInputAssemblyState = &inputAssemblyStateInfo;
    pipelineInfo.pRasterizationState = &rasterizationStateInfo;
    pipelineInfo.pColorBlendState = &colorBlendStateInfo;
    pipelineInfo.pMultisampleState = &multisampleStateInfo;
    pipelineInfo.pViewportState = &viewportStateInfo;
    pipelineInfo.pDepthStencilState = &depthStencilStateInfo;
    pipelineInfo.pDynamicState = &dynamicStateInfo;
#if !(USE_DYNAMIC_RENDERING)
    pipelineInfo.renderPass = renderPass;
    pipelineInfo.subpass = 0;
#endif

    VkPipeline pipeline;
    if (vkCreateGraphicsPipelines(device, nullptr, 1, &pipelineInfo, nullptr, &pipeline) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create main render pipeline!");
    }

    vkDestroyShaderModule(device, vertShaderModule, nullptr);
    vkDestroyShaderModule(device, fragShaderModule, nullptr);

    return pipeline;
}

VkPipeline Renderer_Vulkan::createPrePassPipeline(const std::string& vertShader, const std::string& fragShader) {
    // Shader stages
    auto vertShaderCode = readFile(vertShader);
    auto fragShaderCode = readFile(fragShader);
    auto vertShaderModule = createShaderModule(vertShaderCode);
    auto fragShaderModule = createShaderModule(fragShaderCode);

    VkPipelineShaderStageCreateInfo vertShaderStageInfo{};
    vertShaderStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    vertShaderStageInfo.stage = VK_SHADER_STAGE_VERTEX_BIT;
    vertShaderStageInfo.module = vertShaderModule;
    vertShaderStageInfo.pName = "main";

    VkPipelineShaderStageCreateInfo fragShaderStageInfo{};
    fragShaderStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    fragShaderStageInfo.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    fragShaderStageInfo.module = fragShaderModule;
    fragShaderStageInfo.pName = "main";

    std::array<VkPipelineShaderStageCreateInfo, 2> shaderStages = { vertShaderStageInfo, fragShaderStageInfo };

    // Vertex input state
    std::array<VkVertexInputBindingDescription, 1> vertexBindingDescriptions = {
        { { 0, sizeof(VertexData), VK_VERTEX_INPUT_RATE_VERTEX } }
    };
    std::array<VkVertexInputAttributeDescription, 4> vertexAttributeDescriptions = { {
      { 0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(VertexData, position) },
      { 1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(VertexData, uv) },
      { 2, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(VertexData, normal) },
      { 3, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(VertexData, tangent) },
      // { 4, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(VertexData, bitangent) }
    } };

    VkPipelineVertexInputStateCreateInfo vertexInputStateInfo = {
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO
    };
    vertexInputStateInfo.vertexBindingDescriptionCount = vertexBindingDescriptions.size();
    vertexInputStateInfo.pVertexBindingDescriptions = vertexBindingDescriptions.data();
    vertexInputStateInfo.vertexAttributeDescriptionCount = vertexAttributeDescriptions.size();
    vertexInputStateInfo.pVertexAttributeDescriptions = vertexAttributeDescriptions.data();

    // Input assembly state
    VkPipelineInputAssemblyStateCreateInfo inputAssemblyStateInfo = {
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO
    };
    inputAssemblyStateInfo.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    // Viewport state
    VkViewport viewport = {};
    viewport.x = 0.0f;
    viewport.y = (float)swapchainExtent.height;
    viewport.width = (float)swapchainExtent.width;
    viewport.height = -(float)swapchainExtent.height;
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    VkRect2D scissor = {};
    scissor.offset = { 0, 0 };
    scissor.extent = swapchainExtent;
    VkPipelineViewportStateCreateInfo viewportStateInfo = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    viewportStateInfo.viewportCount = 1;
    viewportStateInfo.pViewports = &viewport;
    viewportStateInfo.scissorCount = 1;
    viewportStateInfo.pScissors = &scissor;

    // Rasterization state
    VkPipelineRasterizationStateCreateInfo rasterizationStateInfo = {
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO
    };
    rasterizationStateInfo.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizationStateInfo.cullMode = VK_CULL_MODE_BACK_BIT;
    rasterizationStateInfo.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterizationStateInfo.depthClampEnable = VK_FALSE;
    rasterizationStateInfo.rasterizerDiscardEnable = VK_FALSE;
    rasterizationStateInfo.depthBiasEnable = VK_FALSE;
    rasterizationStateInfo.lineWidth = 1.0f;

    // Multisample state
    VkPipelineMultisampleStateCreateInfo multisampleStateInfo = {};
    multisampleStateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampleStateInfo.rasterizationSamples = (VkSampleCountFlagBits)MSAA_SAMPLE_COUNT;

    // Depth and stencil state
    VkPipelineDepthStencilStateCreateInfo depthStencilStateInfo = {};
    depthStencilStateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencilStateInfo.depthTestEnable = true;
    depthStencilStateInfo.depthWriteEnable = true;
    depthStencilStateInfo.depthCompareOp = VK_COMPARE_OP_LESS;
    depthStencilStateInfo.depthBoundsTestEnable = false;
    depthStencilStateInfo.stencilTestEnable = false;

    // Color blend state
    VkPipelineColorBlendAttachmentState blendAttachmentState = {};
    blendAttachmentState.colorWriteMask = 0x0;
    blendAttachmentState.blendEnable = VK_FALSE;
    VkPipelineColorBlendStateCreateInfo colorBlendStateInfo = {
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO
    };
    colorBlendStateInfo.attachmentCount = 1;
    colorBlendStateInfo.pAttachments = &blendAttachmentState;

    // Dynamic states
    std::vector<VkDynamicState> enabledDynamicStates = {};
    VkPipelineDynamicStateCreateInfo dynamicStateInfo = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    dynamicStateInfo.dynamicStateCount = static_cast<uint32_t>(enabledDynamicStates.size());
    dynamicStateInfo.pDynamicStates = enabledDynamicStates.data();

    // Pipeline creation
    VkGraphicsPipelineCreateInfo pipelineInfo = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    pipelineInfo.layout = prePassPipelineLayout;
    pipelineInfo.stageCount = static_cast<uint32_t>(shaderStages.size());
    pipelineInfo.pStages = shaderStages.data();
    pipelineInfo.pVertexInputState = &vertexInputStateInfo;
    pipelineInfo.pInputAssemblyState = &inputAssemblyStateInfo;
    pipelineInfo.pRasterizationState = &rasterizationStateInfo;
    pipelineInfo.pColorBlendState = &colorBlendStateInfo;
    pipelineInfo.pMultisampleState = &multisampleStateInfo;
    pipelineInfo.pViewportState = &viewportStateInfo;
    pipelineInfo.pDepthStencilState = &depthStencilStateInfo;
    pipelineInfo.pDynamicState = &dynamicStateInfo;
#if !(USE_DYNAMIC_RENDERING)
    pipelineInfo.renderPass = prePass;
    pipelineInfo.subpass = 0;
#endif

    VkPipeline pipeline;
    if (vkCreateGraphicsPipelines(device, nullptr, 1, &pipelineInfo, nullptr, &pipeline) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create depth-only pipeline!");
    }

    vkDestroyShaderModule(device, vertShaderModule, nullptr);
    vkDestroyShaderModule(device, fragShaderModule, nullptr);

    return pipeline;
}

VkPipeline Renderer_Vulkan::createPostProcessPipeline(const std::string& vertShader, const std::string& fragShader) {
    // Shader stages
    auto vertShaderCode = readFile(vertShader);
    auto fragShaderCode = readFile(fragShader);
    auto vertShaderModule = createShaderModule(vertShaderCode);
    auto fragShaderModule = createShaderModule(fragShaderCode);

    VkPipelineShaderStageCreateInfo vertShaderStageInfo{};
    vertShaderStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    vertShaderStageInfo.stage = VK_SHADER_STAGE_VERTEX_BIT;
    vertShaderStageInfo.module = vertShaderModule;
    vertShaderStageInfo.pName = "main";

    VkPipelineShaderStageCreateInfo fragShaderStageInfo{};
    fragShaderStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    fragShaderStageInfo.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    fragShaderStageInfo.module = fragShaderModule;
    fragShaderStageInfo.pName = "main";

    std::array<VkPipelineShaderStageCreateInfo, 2> shaderStages = { vertShaderStageInfo, fragShaderStageInfo };

    // Vertex input state
    VkPipelineVertexInputStateCreateInfo vertexInputStateInfo = {
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO
    };
    vertexInputStateInfo.vertexBindingDescriptionCount = 0;
    vertexInputStateInfo.pVertexBindingDescriptions = nullptr;
    vertexInputStateInfo.vertexAttributeDescriptionCount = 0;
    vertexInputStateInfo.pVertexAttributeDescriptions = nullptr;

    // Input assembly state
    VkPipelineInputAssemblyStateCreateInfo inputAssemblyStateInfo = {
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO
    };
    inputAssemblyStateInfo.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    // Viewport state
    VkViewport viewport = {};
    viewport.x = 0.0f;
    viewport.y = (float)swapchainExtent.height;
    viewport.width = (float)swapchainExtent.width;
    viewport.height = -(float)swapchainExtent.height;
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    VkRect2D scissor = {};
    scissor.offset = { 0, 0 };
    scissor.extent = swapchainExtent;
    VkPipelineViewportStateCreateInfo viewportStateInfo = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    viewportStateInfo.viewportCount = 1;
    viewportStateInfo.pViewports = &viewport;
    viewportStateInfo.scissorCount = 1;
    viewportStateInfo.pScissors = &scissor;

    // Rasterization state
    VkPipelineRasterizationStateCreateInfo rasterizationStateInfo = {
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO
    };
    rasterizationStateInfo.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizationStateInfo.cullMode = VK_CULL_MODE_NONE;
    rasterizationStateInfo.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterizationStateInfo.depthClampEnable = VK_FALSE;
    rasterizationStateInfo.rasterizerDiscardEnable = VK_FALSE;
    rasterizationStateInfo.depthBiasEnable = VK_FALSE;
    rasterizationStateInfo.lineWidth = 1.0f;

    // Multisample state
    VkPipelineMultisampleStateCreateInfo multisampleStateInfo = {};
    multisampleStateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampleStateInfo.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    // Depth and stencil state
    VkPipelineDepthStencilStateCreateInfo depthStencilStateInfo = {};
    depthStencilStateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencilStateInfo.depthTestEnable = false;
    depthStencilStateInfo.depthWriteEnable = false;
    depthStencilStateInfo.depthCompareOp = VK_COMPARE_OP_LESS;
    depthStencilStateInfo.depthBoundsTestEnable = false;
    depthStencilStateInfo.stencilTestEnable = false;

    // Color blend state
    VkPipelineColorBlendAttachmentState blendAttachmentState = {};
    blendAttachmentState.colorWriteMask = 0xf;
    blendAttachmentState.blendEnable = VK_FALSE;
    VkPipelineColorBlendStateCreateInfo colorBlendStateInfo = {
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO
    };
    colorBlendStateInfo.attachmentCount = 1;
    colorBlendStateInfo.pAttachments = &blendAttachmentState;

    // Dynamic states
    std::vector<VkDynamicState> enabledDynamicStates = {};
    VkPipelineDynamicStateCreateInfo dynamicStateInfo = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    dynamicStateInfo.dynamicStateCount = static_cast<uint32_t>(enabledDynamicStates.size());
    dynamicStateInfo.pDynamicStates = enabledDynamicStates.data();

    // Pipeline creation
    VkGraphicsPipelineCreateInfo pipelineInfo = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    pipelineInfo.layout = postProcessPipelineLayout;
    pipelineInfo.stageCount = static_cast<uint32_t>(shaderStages.size());
    pipelineInfo.pStages = shaderStages.data();
    pipelineInfo.pVertexInputState = &vertexInputStateInfo;
    pipelineInfo.pInputAssemblyState = &inputAssemblyStateInfo;
    pipelineInfo.pRasterizationState = &rasterizationStateInfo;
    pipelineInfo.pColorBlendState = &colorBlendStateInfo;
    pipelineInfo.pMultisampleState = &multisampleStateInfo;
    pipelineInfo.pViewportState = &viewportStateInfo;
    pipelineInfo.pDepthStencilState = &depthStencilStateInfo;
    pipelineInfo.pDynamicState = &dynamicStateInfo;
#if !(USE_DYNAMIC_RENDERING)
    pipelineInfo.renderPass = postProcessPass;
    pipelineInfo.subpass = 0;
#endif

    VkPipeline pipeline;
    if (vkCreateGraphicsPipelines(device, nullptr, 1, &pipelineInfo, nullptr, &pipeline) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create post-process pipeline!");
    }

    vkDestroyShaderModule(device, vertShaderModule, nullptr);
    vkDestroyShaderModule(device, fragShaderModule, nullptr);

    return pipeline;
}

VkPipeline Renderer_Vulkan::createComputePipeline(const std::string& filename, VkPipelineLayout layout) {
    auto shaderCode = readFile(filename);
    auto shaderModule = createShaderModule(shaderCode);
    VkPipelineShaderStageCreateInfo compShaderStageInfo{};
    compShaderStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    compShaderStageInfo.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    compShaderStageInfo.module = shaderModule;
    compShaderStageInfo.pName = "main";

    std::array<VkPipelineShaderStageCreateInfo, 1> shaderStages = { compShaderStageInfo };

    VkComputePipelineCreateInfo pipelineInfo = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
    pipelineInfo.stage = compShaderStageInfo;
    pipelineInfo.layout = layout;

    VkPipeline pipeline;
    if (vkCreateComputePipelines(device, nullptr, 1, &pipelineInfo, nullptr, &pipeline) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create compute pipeline!");
    }

    vkDestroyShaderModule(device, shaderModule, nullptr);

    return pipeline;
}

RenderTargetHandle Renderer_Vulkan::createRenderTarget(RenderTargetUsage usage, VkFormat format) {
    VkImageUsageFlagBits usageFlag;
    VkImageAspectFlagBits aspectFlag;
    VkSampleCountFlagBits sampleCount;
    switch (usage) {
    case RenderTargetUsage::COLOR_MSAA:
        usageFlag = (VkImageUsageFlagBits)(VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        aspectFlag = VK_IMAGE_ASPECT_COLOR_BIT;
        sampleCount = (VkSampleCountFlagBits)MSAA_SAMPLE_COUNT;
        break;
    case RenderTargetUsage::COLOR:
        usageFlag = (VkImageUsageFlagBits)(VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT
                                           | VK_IMAGE_USAGE_SAMPLED_BIT);
        aspectFlag = VK_IMAGE_ASPECT_COLOR_BIT;
        sampleCount = VK_SAMPLE_COUNT_1_BIT;
        break;
    case RenderTargetUsage::DEPTH_MSAA:
        usageFlag =
          (VkImageUsageFlagBits)(VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        aspectFlag = VK_IMAGE_ASPECT_DEPTH_BIT;
        sampleCount = (VkSampleCountFlagBits)MSAA_SAMPLE_COUNT;
        break;
    case RenderTargetUsage::DEPTH_STENCIL_MSAA:
        usageFlag =
          (VkImageUsageFlagBits)(VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        aspectFlag = (VkImageAspectFlagBits)(VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);
        sampleCount = (VkSampleCountFlagBits)MSAA_SAMPLE_COUNT;
        break;
    case RenderTargetUsage::DEPTH:
        usageFlag = (VkImageUsageFlagBits)(VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT
                                           | VK_IMAGE_USAGE_SAMPLED_BIT);
        aspectFlag = VK_IMAGE_ASPECT_DEPTH_BIT;
        sampleCount = VK_SAMPLE_COUNT_1_BIT;
        break;
    case RenderTargetUsage::DEPTH_STENCIL:
        usageFlag = (VkImageUsageFlagBits)(VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT
                                           | VK_IMAGE_USAGE_SAMPLED_BIT);
        aspectFlag = (VkImageAspectFlagBits)(VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);
        sampleCount = VK_SAMPLE_COUNT_1_BIT;
        break;
    default:
        throw std::runtime_error("Unknown image usage!");
    }

    VkImageCreateInfo imageInfo = {};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = format;
    imageInfo.extent = { swapchainExtent.width, swapchainExtent.height, 1 };
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = (VkSampleCountFlagBits)sampleCount;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = usageFlag;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.queueFamilyIndexCount = 0;
    imageInfo.pQueueFamilyIndices = nullptr;
    imageInfo.flags = 0;

    VkImage image;
    if (vkCreateImage(device, &imageInfo, nullptr, &image) != VK_SUCCESS) {
        throw std::runtime_error(fmt::format("Failed to create render target image!\n"));
    }

    VkMemoryRequirements memRequirements;
    vkGetImageMemoryRequirements(device, image, &memRequirements);

    VkPhysicalDeviceMemoryProperties memProperties;
    vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memProperties);
    int memTypeIdx = 0;
    for (; memTypeIdx < memProperties.memoryTypeCount; memTypeIdx++) {
        if ((memRequirements.memoryTypeBits & (1 << memTypeIdx))
            && (memProperties.memoryTypes[memTypeIdx].propertyFlags & (VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))) {
            break;
        }
    }

    VkDeviceMemory imageMemory;
    VkMemoryAllocateInfo allocInfo = {};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memRequirements.size;
    allocInfo.memoryTypeIndex = memTypeIdx;
    if (vkAllocateMemory(device, &allocInfo, nullptr, &imageMemory) != VK_SUCCESS) {
        throw std::runtime_error("Failed to allocate image memory!");
    }
    vkBindImageMemory(device, image, imageMemory, 0);

    VkImageViewCreateInfo imageViewInfo{};
    imageViewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    imageViewInfo.image = image;
    imageViewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    imageViewInfo.format = format;
    imageViewInfo.subresourceRange = { aspectFlag, 0, 1, 0, 1 };

    VkImageView imageView;
    if (vkCreateImageView(device, &imageViewInfo, nullptr, &imageView) != VK_SUCCESS) {
        throw std::runtime_error(fmt::format("Failed to create image view!\n"));
    }

    images[nextImageID] = image;
    imageMemories[nextImageID] = imageMemory;
    imageViews[nextImageID] = imageView;

    return RenderTargetHandle{ nextImageID++ };
}

TextureHandle Renderer_Vulkan::createTexture(std::shared_ptr<Image> img) {
    if (!img) {
        throw std::runtime_error(fmt::format("Failed to create texture at {}!\n", img->uri));
    }

    VkFormat format = img->channelCount == 1 ? VK_FORMAT_R8_UNORM : VK_FORMAT_R8G8B8A8_UNORM;
    int numLevels = calculateMipmapLevelCount(img->width, img->height);
    VkImageCreateInfo imageInfo = {};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = format;
    imageInfo.extent.width = img->width;
    imageInfo.extent.height = img->height;
    imageInfo.extent.depth = 1;
    imageInfo.mipLevels = numLevels;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.queueFamilyIndexCount = 0;
    imageInfo.pQueueFamilyIndices = nullptr;
    imageInfo.flags = 0;

    VkImage image;
    if (vkCreateImage(device, &imageInfo, nullptr, &image) != VK_SUCCESS) {
        throw std::runtime_error(fmt::format("Failed to create image at {}!\n", img->uri));
    }

    VkMemoryRequirements memRequirements;
    vkGetImageMemoryRequirements(device, image, &memRequirements);

    VkPhysicalDeviceMemoryProperties memProperties;
    vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memProperties);
    int memTypeIdx = 0;
    for (; memTypeIdx < memProperties.memoryTypeCount; memTypeIdx++) {
        if ((memRequirements.memoryTypeBits & (1 << memTypeIdx))
            && (memProperties.memoryTypes[memTypeIdx].propertyFlags & (VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))) {
            break;
        }
    }

    VkDeviceMemory imageMemory;
    VkMemoryAllocateInfo allocInfo = {};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memRequirements.size;
    allocInfo.memoryTypeIndex = memTypeIdx;

    if (vkAllocateMemory(device, &allocInfo, nullptr, &imageMemory) != VK_SUCCESS) {
        throw std::runtime_error("Failed to allocate image memory!");
    }
    vkBindImageMemory(device, image, imageMemory, 0);

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = format;
    viewInfo.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, static_cast<uint32_t>(numLevels), 0, 1 };

    VkImageView imageView;
    if (vkCreateImageView(device, &viewInfo, nullptr, &imageView) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create texture image view!");
    }

    VkDeviceSize bufferSize = img->byteArray.size();
    auto stagingBufferHandle = createBuffer(BufferUsage::COPY_SRC, bufferSize);
    VkBuffer stagingBuffer = getBuffer(stagingBufferHandle);// TODO: use internal pointer
    VkDeviceMemory stagingBufferMemory = getBufferMemory(stagingBufferHandle);// TODO: use internal pointer

    void* data;
    vkMapMemory(device, stagingBufferMemory, 0, bufferSize, 0, &data);
    memcpy(data, img->byteArray.data(), static_cast<size_t>(bufferSize));
    vkUnmapMemory(device, stagingBufferMemory);

    VkCommandBufferAllocateInfo commandBufferInfo = {};
    commandBufferInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    commandBufferInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    commandBufferInfo.commandPool = commandPool;
    commandBufferInfo.commandBufferCount = 1;

    VkCommandBuffer cmd;
    vkAllocateCommandBuffers(device, &commandBufferInfo, &cmd);

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

    vkBeginCommandBuffer(cmd, &beginInfo);

    insertImageMemoryBarrier(
      cmd,
      image,
      0,
      VK_ACCESS_TRANSFER_WRITE_BIT,
      VK_IMAGE_LAYOUT_UNDEFINED,
      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
      VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
      VK_PIPELINE_STAGE_TRANSFER_BIT,
      { VK_IMAGE_ASPECT_COLOR_BIT, 0, static_cast<uint32_t>(numLevels), 0, 1 }
    );
    VkBufferImageCopy copyRegion = {};
    copyRegion.bufferOffset = 0;
    copyRegion.bufferRowLength = 0;
    copyRegion.bufferImageHeight = 0;
    copyRegion.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    copyRegion.imageOffset = { 0, 0, 0 };
    copyRegion.imageExtent = { static_cast<uint32_t>(img->width), static_cast<uint32_t>(img->height), 1 };
    vkCmdCopyBufferToImage(cmd, stagingBuffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copyRegion);

    insertImageMemoryBarrier(
      cmd,
      image,
      VK_ACCESS_TRANSFER_WRITE_BIT,
      VK_ACCESS_TRANSFER_READ_BIT,
      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
      VK_PIPELINE_STAGE_TRANSFER_BIT,
      VK_PIPELINE_STAGE_TRANSFER_BIT,
      { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 }
    );
    for (int i = 1; i < numLevels; i++) {
        VkImageBlit blit = {};
        blit.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, static_cast<uint32_t>(i - 1), 0, 1 };
        blit.srcOffsets[0] = { 0, 0, 0 };
        blit.srcOffsets[1] = { static_cast<int32_t>(img->width >> (i - 1)),
                               static_cast<int32_t>(img->height >> (i - 1)),
                               1 };
        blit.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, static_cast<uint32_t>(i), 0, 1 };
        blit.dstOffsets[0] = { 0, 0, 0 };
        blit.dstOffsets[1] = { static_cast<int32_t>(img->width >> i), static_cast<int32_t>(img->height >> i), 1 };
        vkCmdBlitImage(
          cmd,
          image,
          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
          image,
          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
          1,
          &blit,
          VK_FILTER_LINEAR
        );
        insertImageMemoryBarrier(
          cmd,
          image,
          VK_ACCESS_TRANSFER_WRITE_BIT,
          VK_ACCESS_TRANSFER_READ_BIT,
          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
          VK_PIPELINE_STAGE_TRANSFER_BIT,
          VK_PIPELINE_STAGE_TRANSFER_BIT,
          { VK_IMAGE_ASPECT_COLOR_BIT, static_cast<uint32_t>(i), 1, 0, 1 }
        );
    }

    insertImageMemoryBarrier(
      cmd,
      image,
      VK_ACCESS_TRANSFER_READ_BIT,
      VK_ACCESS_SHADER_READ_BIT,
      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
      VK_PIPELINE_STAGE_TRANSFER_BIT,
      VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
      { VK_IMAGE_ASPECT_COLOR_BIT, 0, static_cast<uint32_t>(numLevels), 0, 1 }
    );

    vkEndCommandBuffer(cmd);

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &cmd;

    vkQueueSubmit(graphicsQueue, 1, &submitInfo, VK_NULL_HANDLE);
    vkQueueWaitIdle(graphicsQueue);

    vkFreeCommandBuffers(device, commandPool, 1, &cmd);

    images[nextImageID] = image;
    imageMemories[nextImageID] = imageMemory;
    imageViews[nextImageID] = imageView;

    return TextureHandle{ nextImageID++ };
}

BufferHandle Renderer_Vulkan::createVertexBuffer(std::vector<VertexData> vertices) {
    VkDeviceSize bufferSize = sizeof(VertexData) * vertices.size();
    auto bufferHandle = createBuffer(BufferUsage::VERTEX, bufferSize);
    VkDeviceMemory bufferMemory = getBufferMemory(bufferHandle);// TODO: use internal pointer

    void* data;
    vkMapMemory(device, bufferMemory, 0, bufferSize, 0, &data);
    memcpy(data, vertices.data(), bufferSize);
    vkUnmapMemory(device, bufferMemory);

    return bufferHandle;
}

BufferHandle Renderer_Vulkan::createIndexBuffer(std::vector<Uint32> indices) {
    VkDeviceSize bufferSize = sizeof(Uint32) * indices.size();
    auto bufferHandle = createBuffer(BufferUsage::INDEX, bufferSize);
    VkDeviceMemory bufferMemory = getBufferMemory(bufferHandle);// TODO: use internal pointer

    void* data;
    vkMapMemory(device, bufferMemory, 0, bufferSize, 0, &data);
    memcpy(data, indices.data(), bufferSize);
    vkUnmapMemory(device, bufferMemory);

    return bufferHandle;
}

BufferHandle Renderer_Vulkan::createBuffer(BufferUsage usage, VkDeviceSize size) {
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = size;
    switch (usage) {
    case BufferUsage::VERTEX:
        bufferInfo.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
        break;
    case BufferUsage::INDEX:
        bufferInfo.usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
        break;
    case BufferUsage::UNIFORM:
        bufferInfo.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
        break;
    case BufferUsage::STORAGE:
        bufferInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        break;
    case BufferUsage::COPY_SRC:
        bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        break;
    case BufferUsage::COPY_DST:
        bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        break;
    default:
        throw std::runtime_error("Unknown buffer usage!");
    }
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VkBuffer buffer;
    if (vkCreateBuffer(device, &bufferInfo, nullptr, &buffer) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create buffer!");
    }

    VkMemoryRequirements memRequirements;
    vkGetBufferMemoryRequirements(device, buffer, &memRequirements);

    VkPhysicalDeviceMemoryProperties memProperties;
    vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memProperties);
    int memTypeIdx = 0;
    for (; memTypeIdx < memProperties.memoryTypeCount; memTypeIdx++) {
        if ((memRequirements.memoryTypeBits & (1 << memTypeIdx))
            && (memProperties.memoryTypes[memTypeIdx].propertyFlags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))) {
            break;
        }
    }

    VkDeviceMemory memory;
    VkMemoryAllocateInfo allocInfo = {};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memRequirements.size;
    allocInfo.memoryTypeIndex = memTypeIdx;

    if (vkAllocateMemory(device, &allocInfo, nullptr, &memory) != VK_SUCCESS) {
        throw std::runtime_error("Failed to allocate buffer memory!");
    }
    vkBindBufferMemory(device, buffer, memory, 0);

    buffers[nextBufferID] = buffer;
    bufferMemories[nextBufferID] = memory;

    return BufferHandle{ nextBufferID++ };
}

BufferHandle Renderer_Vulkan::createBufferMapped(BufferUsage usage, VkDeviceSize size, void** mappedDataPtr) {
    auto bufferHandle = createBuffer(usage, size);

    VkDeviceMemory bufferMemory = getBufferMemory(bufferHandle);// TODO: use internal pointer
    vkMapMemory(
      device, bufferMemory, 0, size, 0, mappedDataPtr
    );// NOTE: vkMapMemory might reassign the data pointer, so using pointer of pointer is necessary

    return bufferHandle;
}

VkBuffer Renderer_Vulkan::getBuffer(BufferHandle handle) const {
    return buffers.at(handle.rid);
}

VkDeviceMemory Renderer_Vulkan::getBufferMemory(BufferHandle handle) const {
    return bufferMemories.at(handle.rid);
}

VkImage Renderer_Vulkan::getTexture(TextureHandle handle) const {
    return images.at(handle.rid);
}

VkImageView Renderer_Vulkan::getTextureView(TextureHandle handle) const {
    return imageViews.at(handle.rid);
}

VkDeviceMemory Renderer_Vulkan::getTextureMemory(TextureHandle handle) const {
    return imageMemories.at(handle.rid);
}

VkImage Renderer_Vulkan::getRenderTarget(RenderTargetHandle handle) const {
    return images.at(handle.rid);
}

VkImageView Renderer_Vulkan::getRenderTargetView(RenderTargetHandle handle) const {
    return imageViews.at(handle.rid);
}

VkDeviceMemory Renderer_Vulkan::getRenderTargetMemory(RenderTargetHandle handle) const {
    return imageMemories.at(handle.rid);
}

VkPipeline Renderer_Vulkan::getPipeline(PipelineHandle handle) const {
    return pipelines.at(handle.rid);
}

// ============================================================================
// Particle System Implementation
// ============================================================================

void Renderer_Vulkan::initParticleSystem() {
    if (!particleSystemEnabled) return;

    // Create descriptor set layout for particle compute
    std::array<VkDescriptorSetLayoutBinding, 3> computeBindings = {{
        {
            .binding = 0,
            .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        },
        {
            .binding = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        },
        {
            .binding = 2,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        },
    }};

    VkDescriptorSetLayoutCreateInfo computeLayoutInfo = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = static_cast<uint32_t>(computeBindings.size()),
        .pBindings = computeBindings.data(),
    };

    if (vkCreateDescriptorSetLayout(device, &computeLayoutInfo, nullptr, &particleComputeSetLayout) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create particle compute descriptor set layout!");
    }

    // Create descriptor set layout for particle rendering (set 1 for particle buffer)
    std::array<VkDescriptorSetLayoutBinding, 1> renderBindings = {{
        {
            .binding = 0,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
        },
    }};

    VkDescriptorSetLayoutCreateInfo renderLayoutInfo = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = static_cast<uint32_t>(renderBindings.size()),
        .pBindings = renderBindings.data(),
    };

    if (vkCreateDescriptorSetLayout(device, &renderLayoutInfo, nullptr, &particleRenderSetLayout) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create particle render descriptor set layout!");
    }

    // Create compute pipeline layout
    VkPipelineLayoutCreateInfo computePipelineLayoutInfo = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &particleComputeSetLayout,
    };

    if (vkCreatePipelineLayout(device, &computePipelineLayoutInfo, nullptr, &particleComputePipelineLayout) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create particle compute pipeline layout!");
    }

    // Create render pipeline layout with push constants
    VkPushConstantRange pushConstantRange = {
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
        .offset = 0,
        .size = sizeof(ParticlePushConstants),
    };

    std::array<VkDescriptorSetLayout, 2> renderSetLayouts = { set0Layout, particleRenderSetLayout };

    VkPipelineLayoutCreateInfo renderPipelineLayoutInfo = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = static_cast<uint32_t>(renderSetLayouts.size()),
        .pSetLayouts = renderSetLayouts.data(),
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &pushConstantRange,
    };

    if (vkCreatePipelineLayout(device, &renderPipelineLayoutInfo, nullptr, &particleRenderPipelineLayout) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create particle render pipeline layout!");
    }

    // Create compute pipelines
    particleForcePipeline = createComputePipeline(
        std::string("assets/shaders/ParticleForce.comp.spv"),
        particleComputePipelineLayout
    );

    particleIntegratePipeline = createComputePipeline(
        std::string("assets/shaders/ParticleIntegrate.comp.spv"),
        particleComputePipelineLayout
    );

    // Create particle render pipeline
    auto vertShaderCode = readFile("assets/shaders/Particle.vert.spv");
    auto fragShaderCode = readFile("assets/shaders/Particle.frag.spv");
    auto vertShaderModule = createShaderModule(vertShaderCode);
    auto fragShaderModule = createShaderModule(fragShaderCode);

    VkPipelineShaderStageCreateInfo vertShaderStageInfo = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
        .stage = VK_SHADER_STAGE_VERTEX_BIT,
        .module = vertShaderModule,
        .pName = "main",
    };

    VkPipelineShaderStageCreateInfo fragShaderStageInfo = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
        .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
        .module = fragShaderModule,
        .pName = "main",
    };

    std::array<VkPipelineShaderStageCreateInfo, 2> shaderStages = { vertShaderStageInfo, fragShaderStageInfo };

    // No vertex input - all data comes from storage buffer
    VkPipelineVertexInputStateCreateInfo vertexInputInfo = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = 0,
        .vertexAttributeDescriptionCount = 0,
    };

    VkPipelineInputAssemblyStateCreateInfo inputAssembly = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
        .primitiveRestartEnable = VK_FALSE,
    };

    VkViewport viewport = {
        .x = 0.0f,
        .y = static_cast<float>(swapchainExtent.height),
        .width = static_cast<float>(swapchainExtent.width),
        .height = -static_cast<float>(swapchainExtent.height),
        .minDepth = 0.0f,
        .maxDepth = 1.0f,
    };

    VkRect2D scissor = {
        .offset = { 0, 0 },
        .extent = swapchainExtent,
    };

    VkPipelineViewportStateCreateInfo viewportState = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1,
        .pViewports = &viewport,
        .scissorCount = 1,
        .pScissors = &scissor,
    };

    VkPipelineRasterizationStateCreateInfo rasterizer = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .depthClampEnable = VK_FALSE,
        .rasterizerDiscardEnable = VK_FALSE,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode = VK_CULL_MODE_NONE,
        .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
        .depthBiasEnable = VK_FALSE,
        .lineWidth = 1.0f,
    };

    VkPipelineMultisampleStateCreateInfo multisampling = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = (VkSampleCountFlagBits)MSAA_SAMPLE_COUNT,
        .sampleShadingEnable = VK_FALSE,
    };

    VkPipelineDepthStencilStateCreateInfo depthStencil = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable = VK_TRUE,
        .depthWriteEnable = VK_FALSE, // Particles don't write depth
        .depthCompareOp = VK_COMPARE_OP_LESS,
        .depthBoundsTestEnable = VK_FALSE,
        .stencilTestEnable = VK_FALSE,
    };

    // Additive blending for particles
    VkPipelineColorBlendAttachmentState colorBlendAttachment = {
        .blendEnable = VK_TRUE,
        .srcColorBlendFactor = VK_BLEND_FACTOR_ONE,
        .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR,
        .colorBlendOp = VK_BLEND_OP_ADD,
        .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
        .dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO,
        .alphaBlendOp = VK_BLEND_OP_ADD,
        .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                          VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
    };

    VkPipelineColorBlendStateCreateInfo colorBlending = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .logicOpEnable = VK_FALSE,
        .attachmentCount = 1,
        .pAttachments = &colorBlendAttachment,
    };

    VkGraphicsPipelineCreateInfo pipelineInfo = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = static_cast<uint32_t>(shaderStages.size()),
        .pStages = shaderStages.data(),
        .pVertexInputState = &vertexInputInfo,
        .pInputAssemblyState = &inputAssembly,
        .pViewportState = &viewportState,
        .pRasterizationState = &rasterizer,
        .pMultisampleState = &multisampling,
        .pDepthStencilState = &depthStencil,
        .pColorBlendState = &colorBlending,
        .layout = particleRenderPipelineLayout,
        .renderPass = renderPass,
        .subpass = 0,
    };

    if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &particleRenderPipeline) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create particle render pipeline!");
    }

    vkDestroyShaderModule(device, vertShaderModule, nullptr);
    vkDestroyShaderModule(device, fragShaderModule, nullptr);

    // Create particle buffers
    VkDeviceSize particleBufferSize = sizeof(GPUParticle) * MAX_PARTICLES;
    particleBuffers.resize(MAX_FRAMES_IN_FLIGHT);
    for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        particleBuffers[i] = createBuffer(BufferUsage::STORAGE, particleBufferSize);
    }

    // Create simulation params buffers
    particleSimParamsBuffers.resize(MAX_FRAMES_IN_FLIGHT);
    particleSimParamsBuffersMapped.resize(MAX_FRAMES_IN_FLIGHT);
    for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        particleSimParamsBuffers[i] = createBufferMapped(
            BufferUsage::UNIFORM,
            sizeof(ParticleSimulationParams),
            &particleSimParamsBuffersMapped[i]
        );
    }

    // Create attractor buffers
    particleAttractorBuffers.resize(MAX_FRAMES_IN_FLIGHT);
    particleAttractorBuffersMapped.resize(MAX_FRAMES_IN_FLIGHT);
    for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        particleAttractorBuffers[i] = createBufferMapped(
            BufferUsage::UNIFORM,
            sizeof(ParticleAttractorData),
            &particleAttractorBuffersMapped[i]
        );
    }

    // Initialize particles with random positions and colors
    std::vector<GPUParticle> initialParticles(MAX_PARTICLES);
    std::srand(static_cast<unsigned>(std::time(nullptr)));

    for (size_t i = 0; i < MAX_PARTICLES; i++) {
        float r = std::sqrt(static_cast<float>(std::rand()) / RAND_MAX) * 5.0f;
        float theta = static_cast<float>(std::rand()) / RAND_MAX * 2.0f * 3.14159265f;
        float phi = static_cast<float>(std::rand()) / RAND_MAX * 3.14159265f;

        initialParticles[i].position = glm::vec3(
            r * std::sin(phi) * std::cos(theta),
            r * std::sin(phi) * std::sin(theta),
            r * std::cos(phi)
        );

        glm::vec3 tangent = glm::normalize(glm::cross(
            initialParticles[i].position,
            glm::vec3(0.0f, 1.0f, 0.0f)
        ));
        if (glm::length(tangent) < 0.001f) {
            tangent = glm::vec3(1.0f, 0.0f, 0.0f);
        }
        initialParticles[i].velocity = tangent * (0.5f / (r + 0.1f));

        float brightness = 1.0f - (r / 5.0f);
        glm::vec3 a = glm::vec3(0.427f, 0.346f, 0.372f);
        glm::vec3 b = glm::vec3(0.288f, 0.918f, 0.336f);
        glm::vec3 c = glm::vec3(0.635f, 1.136f, 0.404f);
        glm::vec3 d = glm::vec3(1.893f, 0.663f, 1.910f);
        glm::vec3 color = a + b * glm::cos(6.28318f * (c * brightness + d));
        initialParticles[i].color = glm::vec4(color, 1.0f);
    }

    // Upload initial particle data
    for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        VkDeviceMemory bufferMemory = getBufferMemory(particleBuffers[i]);
        void* data;
        vkMapMemory(device, bufferMemory, 0, particleBufferSize, 0, &data);
        memcpy(data, initialParticles.data(), particleBufferSize);
        vkUnmapMemory(device, bufferMemory);
    }

    // Create descriptor pool for particles
    std::array<VkDescriptorPoolSize, 2> poolSizes = {{
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 2 * static_cast<uint32_t>(MAX_FRAMES_IN_FLIGHT) },
        { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2 * static_cast<uint32_t>(MAX_FRAMES_IN_FLIGHT) },
    }};

    VkDescriptorPoolCreateInfo poolInfo = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 2 * static_cast<uint32_t>(MAX_FRAMES_IN_FLIGHT),
        .poolSizeCount = static_cast<uint32_t>(poolSizes.size()),
        .pPoolSizes = poolSizes.data(),
    };

    if (vkCreateDescriptorPool(device, &poolInfo, nullptr, &particleDescriptorPool) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create particle descriptor pool!");
    }

    // Allocate compute descriptor sets
    std::vector<VkDescriptorSetLayout> computeLayouts(MAX_FRAMES_IN_FLIGHT, particleComputeSetLayout);
    VkDescriptorSetAllocateInfo computeAllocInfo = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = particleDescriptorPool,
        .descriptorSetCount = static_cast<uint32_t>(MAX_FRAMES_IN_FLIGHT),
        .pSetLayouts = computeLayouts.data(),
    };

    particleComputeSets.resize(MAX_FRAMES_IN_FLIGHT);
    if (vkAllocateDescriptorSets(device, &computeAllocInfo, particleComputeSets.data()) != VK_SUCCESS) {
        throw std::runtime_error("Failed to allocate particle compute descriptor sets!");
    }

    // Allocate render descriptor sets
    std::vector<VkDescriptorSetLayout> renderLayouts(MAX_FRAMES_IN_FLIGHT, particleRenderSetLayout);
    VkDescriptorSetAllocateInfo renderAllocInfo = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = particleDescriptorPool,
        .descriptorSetCount = static_cast<uint32_t>(MAX_FRAMES_IN_FLIGHT),
        .pSetLayouts = renderLayouts.data(),
    };

    particleRenderSets.resize(MAX_FRAMES_IN_FLIGHT);
    if (vkAllocateDescriptorSets(device, &renderAllocInfo, particleRenderSets.data()) != VK_SUCCESS) {
        throw std::runtime_error("Failed to allocate particle render descriptor sets!");
    }

    // Update descriptor sets
    for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        VkDescriptorBufferInfo simParamsInfo = {
            .buffer = getBuffer(particleSimParamsBuffers[i]),
            .offset = 0,
            .range = sizeof(ParticleSimulationParams),
        };

        VkDescriptorBufferInfo attractorInfo = {
            .buffer = getBuffer(particleAttractorBuffers[i]),
            .offset = 0,
            .range = sizeof(ParticleAttractorData),
        };

        VkDescriptorBufferInfo particleBufferInfo = {
            .buffer = getBuffer(particleBuffers[i]),
            .offset = 0,
            .range = sizeof(GPUParticle) * MAX_PARTICLES,
        };

        std::array<VkWriteDescriptorSet, 3> computeWrites = {{
            {
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstSet = particleComputeSets[i],
                .dstBinding = 0,
                .dstArrayElement = 0,
                .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                .pBufferInfo = &simParamsInfo,
            },
            {
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstSet = particleComputeSets[i],
                .dstBinding = 1,
                .dstArrayElement = 0,
                .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                .pBufferInfo = &attractorInfo,
            },
            {
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstSet = particleComputeSets[i],
                .dstBinding = 2,
                .dstArrayElement = 0,
                .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                .pBufferInfo = &particleBufferInfo,
            },
        }};

        vkUpdateDescriptorSets(device, static_cast<uint32_t>(computeWrites.size()), computeWrites.data(), 0, nullptr);

        // Update render descriptor set
        VkWriteDescriptorSet renderWrite = {
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = particleRenderSets[i],
            .dstBinding = 0,
            .dstArrayElement = 0,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .pBufferInfo = &particleBufferInfo,
        };

        vkUpdateDescriptorSets(device, 1, &renderWrite, 0, nullptr);
    }

    fmt::print("Particle system initialized with {} particles\n", MAX_PARTICLES);
}

void Renderer_Vulkan::updateParticleSimulation(VkCommandBuffer cmd, float deltaTime, const glm::vec3& attractorPos) {
    if (!particleSystemEnabled) return;

    // Update simulation parameters
    ParticleSimulationParams simParams = {
        .resolution = glm::vec2(swapchainExtent.width, swapchainExtent.height),
        .mousePosition = glm::vec2(0.0f),
        .time = static_cast<float>(SDL_GetTicks()) / 1000.0f,
        .deltaTime = deltaTime,
    };
    memcpy(particleSimParamsBuffersMapped[currentFrameInFlight], &simParams, sizeof(ParticleSimulationParams));

    // Update attractor
    ParticleAttractorData attractor = {
        .position = attractorPos,
        .strength = 5.0f,
    };
    memcpy(particleAttractorBuffersMapped[currentFrameInFlight], &attractor, sizeof(ParticleAttractorData));

    // Memory barrier before compute
    VkMemoryBarrier memoryBarrier = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
    };

    // Force computation pass
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, particleForcePipeline);
    vkCmdBindDescriptorSets(
        cmd,
        VK_PIPELINE_BIND_POINT_COMPUTE,
        particleComputePipelineLayout,
        0, 1,
        &particleComputeSets[currentFrameInFlight],
        0, nullptr
    );
    vkCmdDispatch(cmd, (particleCount + 255) / 256, 1, 1);

    // Barrier between force and integrate
    vkCmdPipelineBarrier(
        cmd,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0,
        1, &memoryBarrier,
        0, nullptr,
        0, nullptr
    );

    // Integration pass
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, particleIntegratePipeline);
    vkCmdBindDescriptorSets(
        cmd,
        VK_PIPELINE_BIND_POINT_COMPUTE,
        particleComputePipelineLayout,
        0, 1,
        &particleComputeSets[currentFrameInFlight],
        0, nullptr
    );
    vkCmdDispatch(cmd, (particleCount + 255) / 256, 1, 1);

    // Barrier before rendering
    vkCmdPipelineBarrier(
        cmd,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_VERTEX_SHADER_BIT,
        0,
        1, &memoryBarrier,
        0, nullptr,
        0, nullptr
    );
}

void Renderer_Vulkan::renderParticles(VkCommandBuffer cmd) {
    if (!particleSystemEnabled) return;

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, particleRenderPipeline);

    std::array<VkDescriptorSet, 2> descriptorSets = {
        set0s[currentFrameInFlight],
        particleRenderSets[currentFrameInFlight]
    };

    vkCmdBindDescriptorSets(
        cmd,
        VK_PIPELINE_BIND_POINT_GRAPHICS,
        particleRenderPipelineLayout,
        0,
        static_cast<uint32_t>(descriptorSets.size()),
        descriptorSets.data(),
        0, nullptr
    );

    ParticlePushConstants pushConstants = {
        .particleSize = 0.02f,
    };

    vkCmdPushConstants(
        cmd,
        particleRenderPipelineLayout,
        VK_SHADER_STAGE_VERTEX_BIT,
        0,
        sizeof(ParticlePushConstants),
        &pushConstants
    );

    // Draw 6 vertices per particle (2 triangles = 1 quad), instanced
    vkCmdDraw(cmd, 6, particleCount, 0, 0);
}

void Renderer_Vulkan::cleanupParticleSystem() {
    if (!particleSystemEnabled) return;

    vkDestroyPipeline(device, particleRenderPipeline, nullptr);
    vkDestroyPipeline(device, particleForcePipeline, nullptr);
    vkDestroyPipeline(device, particleIntegratePipeline, nullptr);

    vkDestroyPipelineLayout(device, particleRenderPipelineLayout, nullptr);
    vkDestroyPipelineLayout(device, particleComputePipelineLayout, nullptr);

    vkDestroyDescriptorSetLayout(device, particleRenderSetLayout, nullptr);
    vkDestroyDescriptorSetLayout(device, particleComputeSetLayout, nullptr);

    vkDestroyDescriptorPool(device, particleDescriptorPool, nullptr);

    // Note: buffers are cleaned up in the main deinit() through the buffers map
}