#include "rhi_vulkan.hpp"
#include "helper.hpp"
#include <fmt/core.h>
#include <cstring>
#include <algorithm>

// ============================================================================
// Constructor / Destructor
// ============================================================================

RHI_Vulkan::RHI_Vulkan() {
}

RHI_Vulkan::~RHI_Vulkan() {
    if (device != VK_NULL_HANDLE) {
        shutdown();
    }
}

// ============================================================================
// Initialization
// ============================================================================

bool RHI_Vulkan::initialize(SDL_Window* windowPtr) {
    window = windowPtr;

    try {
        createInstance();
        createSurface();
        pickPhysicalDevice();
        createLogicalDevice();
        createSwapchain();
        createCommandPool();
        createCommandBuffers();
        createSyncObjects();
    } catch (const std::exception& e) {
        fmt::print("RHI_Vulkan initialization failed: {}\n", e.what());
        return false;
    }

    return true;
}

void RHI_Vulkan::shutdown() {
    if (device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device);
    }

    // Destroy all resources
    for (auto& [id, buffer] : buffers) {
        if (buffer.buffer != VK_NULL_HANDLE) {
            vkDestroyBuffer(device, buffer.buffer, nullptr);
        }
        if (buffer.memory != VK_NULL_HANDLE) {
            vkFreeMemory(device, buffer.memory, nullptr);
        }
    }
    buffers.clear();

    for (auto& [id, texture] : textures) {
        if (texture.view != VK_NULL_HANDLE) {
            vkDestroyImageView(device, texture.view, nullptr);
        }
        if (texture.image != VK_NULL_HANDLE) {
            vkDestroyImage(device, texture.image, nullptr);
        }
        if (texture.memory != VK_NULL_HANDLE) {
            vkFreeMemory(device, texture.memory, nullptr);
        }
    }
    textures.clear();

    for (auto& [id, shader] : shaders) {
        if (shader.module != VK_NULL_HANDLE) {
            vkDestroyShaderModule(device, shader.module, nullptr);
        }
    }
    shaders.clear();

    for (auto& [id, sampler] : samplers) {
        if (sampler.sampler != VK_NULL_HANDLE) {
            vkDestroySampler(device, sampler.sampler, nullptr);
        }
    }
    samplers.clear();

    for (auto& [id, pipeline] : pipelines) {
        if (pipeline.pipeline != VK_NULL_HANDLE) {
            vkDestroyPipeline(device, pipeline.pipeline, nullptr);
        }
        if (pipeline.layout != VK_NULL_HANDLE) {
            vkDestroyPipelineLayout(device, pipeline.layout, nullptr);
        }
    }
    pipelines.clear();

    // Destroy sync objects
    for (auto& semaphore : imageAvailableSemaphores) {
        vkDestroySemaphore(device, semaphore, nullptr);
    }
    for (auto& semaphore : renderFinishedSemaphores) {
        vkDestroySemaphore(device, semaphore, nullptr);
    }
    for (auto& fence : inFlightFences) {
        vkDestroyFence(device, fence, nullptr);
    }

    // Destroy command pool
    if (commandPool != VK_NULL_HANDLE) {
        vkDestroyCommandPool(device, commandPool, nullptr);
    }

    // Destroy swapchain
    for (auto& imageView : swapchainImageViews) {
        vkDestroyImageView(device, imageView, nullptr);
    }
    if (swapchain != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(device, swapchain, nullptr);
    }

    // Destroy device and instance
    if (device != VK_NULL_HANDLE) {
        vkDestroyDevice(device, nullptr);
    }
    if (surface != VK_NULL_HANDLE) {
        vkDestroySurfaceKHR(instance, surface, nullptr);
    }
    if (instance != VK_NULL_HANDLE) {
        vkDestroyInstance(instance, nullptr);
    }
}

void RHI_Vulkan::waitIdle() {
    if (device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device);
    }
}

// ============================================================================
// Resource Creation - Buffer
// ============================================================================

BufferHandle RHI_Vulkan::createBuffer(const BufferDesc& desc) {
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = desc.size;
    bufferInfo.usage = convertBufferUsage(desc.usage);
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VkBuffer buffer;
    if (vkCreateBuffer(device, &bufferInfo, nullptr, &buffer) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create buffer");
    }

    VkMemoryRequirements memRequirements;
    vkGetBufferMemoryRequirements(device, buffer, &memRequirements);

    VkMemoryPropertyFlags memoryProps = 0;
    switch (desc.memoryUsage) {
        case MemoryUsage::GPU:
            memoryProps = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
            break;
        case MemoryUsage::CPU:
            memoryProps = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
            break;
        case MemoryUsage::CPUtoGPU:
            memoryProps = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
            break;
        case MemoryUsage::GPUreadback:
            memoryProps = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
            break;
    }

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memRequirements.size;
    allocInfo.memoryTypeIndex = findMemoryType(memRequirements.memoryTypeBits, memoryProps);

    VkDeviceMemory memory;
    if (vkAllocateMemory(device, &allocInfo, nullptr, &memory) != VK_SUCCESS) {
        vkDestroyBuffer(device, buffer, nullptr);
        throw std::runtime_error("Failed to allocate buffer memory");
    }

    vkBindBufferMemory(device, buffer, memory, 0);

    Uint32 id = nextBufferId++;
    buffers[id] = {buffer, memory, desc.size, false, nullptr};

    return BufferHandle{id};
}

void RHI_Vulkan::destroyBuffer(BufferHandle handle) {
    auto it = buffers.find(handle.id);
    if (it != buffers.end()) {
        if (it->second.buffer != VK_NULL_HANDLE) {
            vkDestroyBuffer(device, it->second.buffer, nullptr);
        }
        if (it->second.memory != VK_NULL_HANDLE) {
            vkFreeMemory(device, it->second.memory, nullptr);
        }
        buffers.erase(it);
    }
}

// ============================================================================
// Resource Creation - Texture
// ============================================================================

TextureHandle RHI_Vulkan::createTexture(const TextureDesc& desc) {
    // Create image
    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent.width = desc.width;
    imageInfo.extent.height = desc.height;
    imageInfo.extent.depth = desc.depth;
    imageInfo.mipLevels = desc.mipLevels;
    imageInfo.arrayLayers = desc.arrayLayers;
    imageInfo.format = convertPixelFormat(desc.format);
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage = convertTextureUsage(desc.usage);
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VkImage image;
    if (vkCreateImage(device, &imageInfo, nullptr, &image) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create image");
    }

    VkMemoryRequirements memRequirements;
    vkGetImageMemoryRequirements(device, image, &memRequirements);

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memRequirements.size;
    allocInfo.memoryTypeIndex = findMemoryType(memRequirements.memoryTypeBits,
                                                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    VkDeviceMemory memory;
    if (vkAllocateMemory(device, &allocInfo, nullptr, &memory) != VK_SUCCESS) {
        vkDestroyImage(device, image, nullptr);
        throw std::runtime_error("Failed to allocate image memory");
    }

    vkBindImageMemory(device, image, memory, 0);

    // Create image view
    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = convertPixelFormat(desc.format);
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = desc.mipLevels;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = desc.arrayLayers;

    VkImageView view;
    if (vkCreateImageView(device, &viewInfo, nullptr, &view) != VK_SUCCESS) {
        vkFreeMemory(device, memory, nullptr);
        vkDestroyImage(device, image, nullptr);
        throw std::runtime_error("Failed to create image view");
    }

    Uint32 id = nextTextureId++;
    textures[id] = {image, view, memory, convertPixelFormat(desc.format), desc.width, desc.height};

    return TextureHandle{id};
}

void RHI_Vulkan::destroyTexture(TextureHandle handle) {
    auto it = textures.find(handle.id);
    if (it != textures.end()) {
        if (it->second.view != VK_NULL_HANDLE) {
            vkDestroyImageView(device, it->second.view, nullptr);
        }
        if (it->second.image != VK_NULL_HANDLE) {
            vkDestroyImage(device, it->second.image, nullptr);
        }
        if (it->second.memory != VK_NULL_HANDLE) {
            vkFreeMemory(device, it->second.memory, nullptr);
        }
        textures.erase(it);
    }
}

// ============================================================================
// Resource Creation - Shader
// ============================================================================

ShaderHandle RHI_Vulkan::createShader(const ShaderDesc& desc) {
    VkShaderModuleCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    createInfo.codeSize = desc.codeSize;
    createInfo.pCode = reinterpret_cast<const uint32_t*>(desc.code);

    VkShaderModule module;
    if (vkCreateShaderModule(device, &createInfo, nullptr, &module) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create shader module");
    }

    Uint32 id = nextShaderId++;
    shaders[id] = {module, desc.stage};

    return ShaderHandle{id};
}

void RHI_Vulkan::destroyShader(ShaderHandle handle) {
    auto it = shaders.find(handle.id);
    if (it != shaders.end()) {
        if (it->second.module != VK_NULL_HANDLE) {
            vkDestroyShaderModule(device, it->second.module, nullptr);
        }
        shaders.erase(it);
    }
}

// ============================================================================
// Resource Creation - Sampler
// ============================================================================

SamplerHandle RHI_Vulkan::createSampler(const SamplerDesc& desc) {
    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = convertFilterMode(desc.magFilter);
    samplerInfo.minFilter = convertFilterMode(desc.minFilter);
    samplerInfo.mipmapMode = desc.mipFilter == FilterMode::Linear
                              ? VK_SAMPLER_MIPMAP_MODE_LINEAR
                              : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerInfo.addressModeU = convertAddressMode(desc.addressModeU);
    samplerInfo.addressModeV = convertAddressMode(desc.addressModeV);
    samplerInfo.addressModeW = convertAddressMode(desc.addressModeW);
    samplerInfo.mipLodBias = desc.mipLodBias;
    samplerInfo.anisotropyEnable = desc.enableAnisotropy ? VK_TRUE : VK_FALSE;
    samplerInfo.maxAnisotropy = desc.maxAnisotropy;
    samplerInfo.compareEnable = desc.enableCompare ? VK_TRUE : VK_FALSE;
    samplerInfo.compareOp = convertCompareOp(desc.compareOp);
    samplerInfo.minLod = desc.minLod;
    samplerInfo.maxLod = desc.maxLod;
    samplerInfo.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
    samplerInfo.unnormalizedCoordinates = VK_FALSE;

    VkSampler sampler;
    if (vkCreateSampler(device, &samplerInfo, nullptr, &sampler) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create sampler");
    }

    Uint32 id = nextSamplerId++;
    samplers[id] = {sampler};

    return SamplerHandle{id};
}

void RHI_Vulkan::destroySampler(SamplerHandle handle) {
    auto it = samplers.find(handle.id);
    if (it != samplers.end()) {
        if (it->second.sampler != VK_NULL_HANDLE) {
            vkDestroySampler(device, it->second.sampler, nullptr);
        }
        samplers.erase(it);
    }
}

// ============================================================================
// Resource Creation - Pipeline
// ============================================================================

PipelineHandle RHI_Vulkan::createPipeline(const PipelineDesc& desc) {
    // TODO: Implement pipeline creation
    // This is a complex operation that requires:
    // - Shader stage setup
    // - Vertex input state
    // - Input assembly state
    // - Viewport and scissor
    // - Rasterization state
    // - Multisample state
    // - Depth/stencil state
    // - Color blend state
    // - Pipeline layout
    // - Render pass compatibility

    fmt::print("Warning: Pipeline creation not yet implemented\n");

    Uint32 id = nextPipelineId++;
    pipelines[id] = {VK_NULL_HANDLE, VK_NULL_HANDLE};

    return PipelineHandle{id};
}

void RHI_Vulkan::destroyPipeline(PipelineHandle handle) {
    auto it = pipelines.find(handle.id);
    if (it != pipelines.end()) {
        if (it->second.pipeline != VK_NULL_HANDLE) {
            vkDestroyPipeline(device, it->second.pipeline, nullptr);
        }
        if (it->second.layout != VK_NULL_HANDLE) {
            vkDestroyPipelineLayout(device, it->second.layout, nullptr);
        }
        pipelines.erase(it);
    }
}

// ============================================================================
// Resource Updates
// ============================================================================

void RHI_Vulkan::updateBuffer(BufferHandle handle, const void* data, size_t offset, size_t size) {
    auto it = buffers.find(handle.id);
    if (it == buffers.end()) {
        return;
    }

    void* mapped;
    vkMapMemory(device, it->second.memory, offset, size, 0, &mapped);
    std::memcpy(mapped, data, size);
    vkUnmapMemory(device, it->second.memory);
}

void RHI_Vulkan::updateTexture(TextureHandle handle, const void* data, size_t size) {
    // TODO: Implement texture update
    // This requires:
    // - Creating a staging buffer
    // - Copying data to staging buffer
    // - Recording copy command
    // - Transitioning image layout
    fmt::print("Warning: Texture update not yet implemented\n");
}

// ============================================================================
// Frame Operations
// ============================================================================

void RHI_Vulkan::beginFrame() {
    vkWaitForFences(device, 1, &inFlightFences[currentFrameInFlight], VK_TRUE, UINT64_MAX);

    VkResult result = vkAcquireNextImageKHR(device, swapchain, UINT64_MAX,
                                            imageAvailableSemaphores[currentFrameInFlight],
                                            VK_NULL_HANDLE, &currentSwapchainImageIndex);

    if (result == VK_ERROR_OUT_OF_DATE_KHR) {
        // Swapchain needs recreation
        return;
    }

    vkResetFences(device, 1, &inFlightFences[currentFrameInFlight]);

    currentCommandBuffer = commandBuffers[currentFrameInFlight];
    vkResetCommandBuffer(currentCommandBuffer, 0);

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    vkBeginCommandBuffer(currentCommandBuffer, &beginInfo);
}

void RHI_Vulkan::endFrame() {
    vkEndCommandBuffer(currentCommandBuffer);

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;

    VkSemaphore waitSemaphores[] = {imageAvailableSemaphores[currentFrameInFlight]};
    VkPipelineStageFlags waitStages[] = {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
    submitInfo.waitSemaphoreCount = 1;
    submitInfo.pWaitSemaphores = waitSemaphores;
    submitInfo.pWaitDstStageMask = waitStages;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &currentCommandBuffer;

    VkSemaphore signalSemaphores[] = {renderFinishedSemaphores[currentFrameInFlight]};
    submitInfo.signalSemaphoreCount = 1;
    submitInfo.pSignalSemaphores = signalSemaphores;

    vkQueueSubmit(graphicsQueue, 1, &submitInfo, inFlightFences[currentFrameInFlight]);

    VkPresentInfoKHR presentInfo{};
    presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pWaitSemaphores = signalSemaphores;

    VkSwapchainKHR swapchains[] = {swapchain};
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = swapchains;
    presentInfo.pImageIndices = &currentSwapchainImageIndex;

    vkQueuePresentKHR(presentQueue, &presentInfo);

    currentFrameInFlight = (currentFrameInFlight + 1) % MAX_FRAMES_IN_FLIGHT;
}

void RHI_Vulkan::beginRenderPass(const RenderPassDesc& desc) {
    // TODO: Implement render pass begin
    fmt::print("Warning: beginRenderPass not yet implemented\n");
}

void RHI_Vulkan::endRenderPass() {
    // TODO: Implement render pass end
}

// ============================================================================
// Rendering Commands
// ============================================================================

void RHI_Vulkan::bindPipeline(PipelineHandle pipeline) {
    // TODO: Implement
}

void RHI_Vulkan::bindVertexBuffer(BufferHandle buffer, Uint32 binding, size_t offset) {
    auto it = buffers.find(buffer.id);
    if (it != buffers.end() && currentCommandBuffer != VK_NULL_HANDLE) {
        VkDeviceSize offsets[] = {offset};
        vkCmdBindVertexBuffers(currentCommandBuffer, binding, 1, &it->second.buffer, offsets);
    }
}

void RHI_Vulkan::bindIndexBuffer(BufferHandle buffer, size_t offset) {
    auto it = buffers.find(buffer.id);
    if (it != buffers.end() && currentCommandBuffer != VK_NULL_HANDLE) {
        vkCmdBindIndexBuffer(currentCommandBuffer, it->second.buffer, offset, VK_INDEX_TYPE_UINT32);
    }
}

void RHI_Vulkan::setUniformBuffer(Uint32 set, Uint32 binding, BufferHandle buffer, size_t offset, size_t range) {
    // TODO: Implement descriptor set binding
}

void RHI_Vulkan::setStorageBuffer(Uint32 set, Uint32 binding, BufferHandle buffer, size_t offset, size_t range) {
    // TODO: Implement descriptor set binding
}

void RHI_Vulkan::setTexture(Uint32 set, Uint32 binding, TextureHandle texture, SamplerHandle sampler) {
    // TODO: Implement descriptor set binding
}

void RHI_Vulkan::draw(Uint32 vertexCount, Uint32 instanceCount, Uint32 firstVertex, Uint32 firstInstance) {
    if (currentCommandBuffer != VK_NULL_HANDLE) {
        vkCmdDraw(currentCommandBuffer, vertexCount, instanceCount, firstVertex, firstInstance);
    }
}

void RHI_Vulkan::drawIndexed(Uint32 indexCount, Uint32 instanceCount, Uint32 firstIndex, int32_t vertexOffset, Uint32 firstInstance) {
    if (currentCommandBuffer != VK_NULL_HANDLE) {
        vkCmdDrawIndexed(currentCommandBuffer, indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);
    }
}

// ============================================================================
// Utility
// ============================================================================

Uint32 RHI_Vulkan::getSwapchainWidth() const {
    return swapchainExtent.width;
}

Uint32 RHI_Vulkan::getSwapchainHeight() const {
    return swapchainExtent.height;
}

PixelFormat RHI_Vulkan::getSwapchainFormat() const {
    // TODO: Convert VkFormat back to PixelFormat
    return PixelFormat::BGRA8_UNORM;
}

// ============================================================================
// Internal Helpers - Initialization (Simplified stubs)
// ============================================================================

void RHI_Vulkan::createInstance() {
    // TODO: Implement full instance creation
    // For now, this is a stub
    fmt::print("Warning: createInstance stub\n");
}

void RHI_Vulkan::createSurface() {
    // TODO: Implement surface creation
    fmt::print("Warning: createSurface stub\n");
}

void RHI_Vulkan::pickPhysicalDevice() {
    // TODO: Implement physical device selection
    fmt::print("Warning: pickPhysicalDevice stub\n");
}

void RHI_Vulkan::createLogicalDevice() {
    // TODO: Implement logical device creation
    fmt::print("Warning: createLogicalDevice stub\n");
}

void RHI_Vulkan::createSwapchain() {
    // TODO: Implement swapchain creation
    fmt::print("Warning: createSwapchain stub\n");
}

void RHI_Vulkan::createCommandPool() {
    // TODO: Implement command pool creation
    fmt::print("Warning: createCommandPool stub\n");
}

void RHI_Vulkan::createCommandBuffers() {
    // TODO: Implement command buffer allocation
    commandBuffers.resize(MAX_FRAMES_IN_FLIGHT);
}

void RHI_Vulkan::createSyncObjects() {
    imageAvailableSemaphores.resize(MAX_FRAMES_IN_FLIGHT);
    renderFinishedSemaphores.resize(MAX_FRAMES_IN_FLIGHT);
    inFlightFences.resize(MAX_FRAMES_IN_FLIGHT);

    VkSemaphoreCreateInfo semaphoreInfo{};
    semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

    for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        if (vkCreateSemaphore(device, &semaphoreInfo, nullptr, &imageAvailableSemaphores[i]) != VK_SUCCESS ||
            vkCreateSemaphore(device, &semaphoreInfo, nullptr, &renderFinishedSemaphores[i]) != VK_SUCCESS ||
            vkCreateFence(device, &fenceInfo, nullptr, &inFlightFences[i]) != VK_SUCCESS) {
            throw std::runtime_error("Failed to create sync objects");
        }
    }
}

// ============================================================================
// Internal Helpers - Conversion Functions
// ============================================================================

Uint32 RHI_Vulkan::findMemoryType(Uint32 typeFilter, VkMemoryPropertyFlags properties) {
    VkPhysicalDeviceMemoryProperties memProperties;
    vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memProperties);

    for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++) {
        if ((typeFilter & (1 << i)) &&
            (memProperties.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }

    throw std::runtime_error("Failed to find suitable memory type");
}

VkFormat RHI_Vulkan::convertPixelFormat(PixelFormat format) {
    switch (format) {
        case PixelFormat::RGBA8_UNORM: return VK_FORMAT_R8G8B8A8_UNORM;
        case PixelFormat::RGBA8_SRGB: return VK_FORMAT_R8G8B8A8_SRGB;
        case PixelFormat::RGBA16_FLOAT: return VK_FORMAT_R16G16B16A16_SFLOAT;
        case PixelFormat::RGBA32_FLOAT: return VK_FORMAT_R32G32B32A32_SFLOAT;
        case PixelFormat::BGRA8_UNORM: return VK_FORMAT_B8G8R8A8_UNORM;
        case PixelFormat::BGRA8_SRGB: return VK_FORMAT_B8G8R8A8_SRGB;
        case PixelFormat::R8_UNORM: return VK_FORMAT_R8_UNORM;
        case PixelFormat::R16_FLOAT: return VK_FORMAT_R16_SFLOAT;
        case PixelFormat::R32_FLOAT: return VK_FORMAT_R32_SFLOAT;
        case PixelFormat::Depth32Float: return VK_FORMAT_D32_SFLOAT;
        case PixelFormat::Depth24Stencil8: return VK_FORMAT_D24_UNORM_S8_UINT;
        default: return VK_FORMAT_R8G8B8A8_UNORM;
    }
}

VkFilter RHI_Vulkan::convertFilterMode(FilterMode mode) {
    switch (mode) {
        case FilterMode::Nearest: return VK_FILTER_NEAREST;
        case FilterMode::Linear: return VK_FILTER_LINEAR;
        default: return VK_FILTER_LINEAR;
    }
}

VkSamplerAddressMode RHI_Vulkan::convertAddressMode(AddressMode mode) {
    switch (mode) {
        case AddressMode::Repeat: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
        case AddressMode::ClampToEdge: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        case AddressMode::ClampToBorder: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        case AddressMode::MirrorRepeat: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
        default: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    }
}

VkCompareOp RHI_Vulkan::convertCompareOp(CompareOp op) {
    switch (op) {
        case CompareOp::Never: return VK_COMPARE_OP_NEVER;
        case CompareOp::Less: return VK_COMPARE_OP_LESS;
        case CompareOp::Equal: return VK_COMPARE_OP_EQUAL;
        case CompareOp::LessOrEqual: return VK_COMPARE_OP_LESS_OR_EQUAL;
        case CompareOp::Greater: return VK_COMPARE_OP_GREATER;
        case CompareOp::NotEqual: return VK_COMPARE_OP_NOT_EQUAL;
        case CompareOp::GreaterOrEqual: return VK_COMPARE_OP_GREATER_OR_EQUAL;
        case CompareOp::Always: return VK_COMPARE_OP_ALWAYS;
        default: return VK_COMPARE_OP_LESS;
    }
}

VkPrimitiveTopology RHI_Vulkan::convertPrimitiveTopology(PrimitiveTopology topology) {
    switch (topology) {
        case PrimitiveTopology::PointList: return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
        case PrimitiveTopology::LineList: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
        case PrimitiveTopology::LineStrip: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
        case PrimitiveTopology::TriangleList: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        case PrimitiveTopology::TriangleStrip: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
        default: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    }
}

VkCullModeFlags RHI_Vulkan::convertCullMode(CullMode mode) {
    switch (mode) {
        case CullMode::None: return VK_CULL_MODE_NONE;
        case CullMode::Front: return VK_CULL_MODE_FRONT_BIT;
        case CullMode::Back: return VK_CULL_MODE_BACK_BIT;
        default: return VK_CULL_MODE_BACK_BIT;
    }
}

VkBufferUsageFlags RHI_Vulkan::convertBufferUsage(BufferUsage usage) {
    switch (usage) {
        case BufferUsage::Vertex: return VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
        case BufferUsage::Index: return VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
        case BufferUsage::Uniform: return VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
        case BufferUsage::Storage: return VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        case BufferUsage::TransferSrc: return VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        case BufferUsage::TransferDst: return VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        default: return VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    }
}

VkImageUsageFlags RHI_Vulkan::convertTextureUsage(TextureUsage usage) {
    switch (usage) {
        case TextureUsage::Sampled: return VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        case TextureUsage::Storage: return VK_IMAGE_USAGE_STORAGE_BIT;
        case TextureUsage::RenderTarget: return VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        case TextureUsage::DepthStencil: return VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        default: return VK_IMAGE_USAGE_SAMPLED_BIT;
    }
}

// ============================================================================
// Factory Function
// ============================================================================

RHI* createRHIVulkan() {
    return new RHI_Vulkan();
}
