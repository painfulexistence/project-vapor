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
        createDescriptorInfrastructure();
        createUploadStream();
        createTimestampPools();
    } catch (const std::exception& e) {
        fmt::print("RHI_Vulkan initialization failed: {}\n", e.what());
        return false;
    }

    // Compute pipelines + resource binding are implemented; raytracing
    // (VK_KHR_acceleration_structure) is not yet.
    capabilities.computeShaders = true;
    capabilities.gpuTimestamps = gpuTimingSupported;

    return true;
}

// ============================================================================
// Batched Upload Stream
// ============================================================================

void RHI_Vulkan::createUploadStream() {
    VkBufferCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size = STAGING_RING_SIZE;
    info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(device, &info, nullptr, &stagingRingBuffer) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create staging ring buffer");
    }
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(device, stagingRingBuffer, &req);
    VkMemoryAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = findMemoryType(req.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (vkAllocateMemory(device, &alloc, nullptr, &stagingRingMemory) != VK_SUCCESS) {
        throw std::runtime_error("Failed to allocate staging ring memory");
    }
    vkBindBufferMemory(device, stagingRingBuffer, stagingRingMemory, 0);
    vkMapMemory(device, stagingRingMemory, 0, STAGING_RING_SIZE, 0, &stagingRingPtr);  // persistent
}

void RHI_Vulkan::destroyUploadStream() {
    submitUploads(true);
    for (VkFence f : pendingUploadFences) vkDestroyFence(device, f, nullptr);
    pendingUploadFences.clear();
    if (stagingRingMemory != VK_NULL_HANDLE) {
        vkUnmapMemory(device, stagingRingMemory);
        vkFreeMemory(device, stagingRingMemory, nullptr);
        stagingRingMemory = VK_NULL_HANDLE;
        stagingRingPtr = nullptr;
    }
    if (stagingRingBuffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(device, stagingRingBuffer, nullptr);
        stagingRingBuffer = VK_NULL_HANDLE;
    }
}

VkCommandBuffer RHI_Vulkan::ensureUploadCmd() {
    if (uploadCmd != VK_NULL_HANDLE) {
        return uploadCmd;
    }
    VkCommandBufferAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc.commandPool = commandPool;
    alloc.commandBufferCount = 1;
    vkAllocateCommandBuffers(device, &alloc, &uploadCmd);
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(uploadCmd, &begin);
    return uploadCmd;
}

void* RHI_Vulkan::allocStaging(VkDeviceSize size, VkDeviceSize& outOffset) {
    // bufferOffset for image copies must be texel-aligned; 16 covers all formats
    VkDeviceSize aligned = (stagingRingOffset + 15) & ~VkDeviceSize(15);
    if (aligned + size > STAGING_RING_SIZE) {
        // Ring exhausted: submit what we have and wait so the space is free
        submitUploads(true);
        aligned = 0;
    }
    outOffset = aligned;
    stagingRingOffset = aligned + size;
    return static_cast<char*>(stagingRingPtr) + aligned;
}

VkBuffer RHI_Vulkan::stageData(const void* data, VkDeviceSize size, VkDeviceSize& outOffset) {
    if (size <= STAGING_RING_SIZE) {
        void* dst = allocStaging(size, outOffset);
        std::memcpy(dst, data, size);
        return stagingRingBuffer;
    }

    // Larger than the whole ring (e.g. a 4096x4096 RGBA texture): dedicated
    // one-shot staging buffer, retired through the deferred-destruction queue
    VkBufferCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size = size;
    info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkBuffer buf;
    if (vkCreateBuffer(device, &info, nullptr, &buf) != VK_SUCCESS) {
        throw std::runtime_error("stageData: failed to create oversize staging buffer");
    }
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(device, buf, &req);
    VkMemoryAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = findMemoryType(req.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkDeviceMemory mem;
    if (vkAllocateMemory(device, &alloc, nullptr, &mem) != VK_SUCCESS) {
        vkDestroyBuffer(device, buf, nullptr);
        throw std::runtime_error("stageData: failed to allocate oversize staging memory");
    }
    vkBindBufferMemory(device, buf, mem, 0);
    void* mapped;
    vkMapMemory(device, mem, 0, size, 0, &mapped);
    std::memcpy(mapped, data, size);
    vkUnmapMemory(device, mem);

    VkDevice dev = device;
    deferDestroy([dev, buf, mem]() {
        vkDestroyBuffer(dev, buf, nullptr);
        vkFreeMemory(dev, mem, nullptr);
    });
    outOffset = 0;
    return buf;
}

void RHI_Vulkan::submitUploads(bool waitForCompletion) {
    if (uploadCmd != VK_NULL_HANDLE) {
        // Make transfer writes visible to subsequent vertex/index/shader reads
        VkMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        vkCmdPipelineBarrier(uploadCmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
        vkEndCommandBuffer(uploadCmd);

        VkFenceCreateInfo fenceInfo{};
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        VkFence fence;
        vkCreateFence(device, &fenceInfo, nullptr, &fence);

        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &uploadCmd;
        vkQueueSubmit(graphicsQueue, 1, &submit, fence);
        pendingUploadFences.push_back(fence);

        // The one-time command buffer is retired with the fence wait below;
        // freeing it later is handled through the retirement queue.
        VkCommandBuffer retiredCmd = uploadCmd;
        VkCommandPool pool = commandPool;
        VkDevice dev = device;
        deferDestroy([dev, pool, retiredCmd]() {
            vkFreeCommandBuffers(dev, pool, 1, &retiredCmd);
        });
        uploadCmd = VK_NULL_HANDLE;
    }

    if (waitForCompletion && !pendingUploadFences.empty()) {
        vkWaitForFences(device, static_cast<uint32_t>(pendingUploadFences.size()),
                        pendingUploadFences.data(), VK_TRUE, UINT64_MAX);
        for (VkFence f : pendingUploadFences) vkDestroyFence(device, f, nullptr);
        pendingUploadFences.clear();
        stagingRingOffset = 0;
    }
}

void RHI_Vulkan::flushUploads() {
    submitUploads(true);
}

// ============================================================================
// GPU Pass Timing
// ============================================================================

void RHI_Vulkan::createTimestampPools() {
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(physicalDevice, &props);
    timestampPeriodNs = props.limits.timestampPeriod;

    uint32_t familyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &familyCount, nullptr);
    std::vector<VkQueueFamilyProperties> families(familyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &familyCount, families.data());
    if (graphicsFamilyIdx >= familyCount ||
        families[graphicsFamilyIdx].timestampValidBits == 0 || timestampPeriodNs <= 0.0f) {
        return;  // timestamps unsupported on this queue
    }

    timestampPools.resize(MAX_FRAMES_IN_FLIGHT, VK_NULL_HANDLE);
    slotTimestamps.resize(MAX_FRAMES_IN_FLIGHT);
    for (auto& pool : timestampPools) {
        VkQueryPoolCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        info.queryType = VK_QUERY_TYPE_TIMESTAMP;
        info.queryCount = TIMESTAMP_QUERIES_PER_POOL;
        if (vkCreateQueryPool(device, &info, nullptr, &pool) != VK_SUCCESS) {
            return;  // leave gpuTimingSupported false; created pools freed at shutdown
        }
    }
    gpuTimingSupported = true;
}

bool RHI_Vulkan::allocateTimestampPair(const char* passName, Uint32& outBegin, Uint32& outEnd) {
    if (!gpuTimingActiveThisFrame || currentCommandBuffer == VK_NULL_HANDLE) {
        return false;
    }
    if (nextTimestampQuery + 2 > TIMESTAMP_QUERIES_PER_POOL) {
        return false;  // budget exhausted; pass goes untimed
    }
    outBegin = nextTimestampQuery;
    outEnd = nextTimestampQuery + 1;
    nextTimestampQuery += 2;
    slotTimestamps[currentFrameInFlight].push_back({ passName ? passName : "(unnamed pass)", outBegin, outEnd });
    return true;
}

void RHI_Vulkan::collectTimestamps(Uint32 slot) {
    auto& infos = slotTimestamps[slot];
    if (infos.empty()) {
        return;
    }
    Uint32 queryCount = infos.back().endQuery + 1;
    std::vector<uint64_t> results(queryCount);
    // The slot's fence has been waited on, so results are ready
    VkResult r = vkGetQueryPoolResults(device, timestampPools[slot], 0, queryCount,
                                       results.size() * sizeof(uint64_t), results.data(),
                                       sizeof(uint64_t), VK_QUERY_RESULT_64_BIT);
    if (r == VK_SUCCESS) {
        std::lock_guard<std::mutex> lock(gpuTimingMutex);
        gpuPassTimings.clear();
        gpuPassTimings.reserve(infos.size());
        for (const auto& info : infos) {
            uint64_t begin = results[info.beginQuery];
            uint64_t end = results[info.endQuery];
            double ms = end >= begin
                ? static_cast<double>(end - begin) * timestampPeriodNs / 1e6
                : 0.0;
            gpuPassTimings.push_back({ info.name, ms });
        }
    }
    infos.clear();
}

std::vector<GpuPassTiming> RHI_Vulkan::getGpuPassTimings() {
    std::lock_guard<std::mutex> lock(gpuTimingMutex);
    return gpuPassTimings;
}

// ============================================================================
// Debug Labels (no-ops when VK_EXT_debug_utils is unavailable)
// ============================================================================

void RHI_Vulkan::beginDebugLabel(VkCommandBuffer cmd, const char* name) {
    if (pfnCmdBeginDebugLabel && cmd != VK_NULL_HANDLE && name) {
        VkDebugUtilsLabelEXT label{};
        label.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT;
        label.pLabelName = name;
        pfnCmdBeginDebugLabel(cmd, &label);
    }
}

void RHI_Vulkan::endDebugLabel(VkCommandBuffer cmd) {
    if (pfnCmdEndDebugLabel && cmd != VK_NULL_HANDLE) {
        pfnCmdEndDebugLabel(cmd);
    }
}

// ============================================================================
// Deferred Destruction
// ============================================================================

void RHI_Vulkan::deferDestroy(std::function<void()> destroy) {
    retirementQueue.push_back({ frameCounter, std::move(destroy) });
}

void RHI_Vulkan::processRetirements(bool force) {
    // Frame N's fence is waited MAX_FRAMES_IN_FLIGHT frames later, so entries
    // queued during frame K are safe once frameCounter >= K + MAX_FRAMES_IN_FLIGHT.
    while (!retirementQueue.empty() &&
           (force || retirementQueue.front().first + MAX_FRAMES_IN_FLIGHT <= frameCounter)) {
        retirementQueue.front().second();
        retirementQueue.pop_front();
    }
}

// ============================================================================
// Descriptor Infrastructure (see header for the binding-model overview)
// ============================================================================

void RHI_Vulkan::createDescriptorInfrastructure() {
    // Set layouts: 8 SSBO slots for each buffer set (both stages visible so a
    // fragment shader can also read vertex-stage data like materials), and 8
    // combined image samplers for the texture set.
    auto makeBufferSetLayout = [&]() {
        VkDescriptorSetLayoutBinding bindings[BINDINGS_PER_SET];
        for (Uint32 i = 0; i < BINDINGS_PER_SET; i++) {
            bindings[i] = {};
            bindings[i].binding = i;
            bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            bindings[i].descriptorCount = 1;
            bindings[i].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        }
        VkDescriptorSetLayoutCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        info.bindingCount = BINDINGS_PER_SET;
        info.pBindings = bindings;
        VkDescriptorSetLayout layout;
        if (vkCreateDescriptorSetLayout(device, &info, nullptr, &layout) != VK_SUCCESS) {
            throw std::runtime_error("Failed to create buffer descriptor set layout");
        }
        return layout;
    };
    vertexBufferSetLayout = makeBufferSetLayout();
    fragmentBufferSetLayout = makeBufferSetLayout();

    {
        VkDescriptorSetLayoutBinding bindings[BINDINGS_PER_SET];
        for (Uint32 i = 0; i < BINDINGS_PER_SET; i++) {
            bindings[i] = {};
            bindings[i].binding = i;
            bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            bindings[i].descriptorCount = 1;
            bindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        }
        VkDescriptorSetLayoutCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        info.bindingCount = BINDINGS_PER_SET;
        info.pBindings = bindings;
        if (vkCreateDescriptorSetLayout(device, &info, nullptr, &textureSetLayout) != VK_SUCCESS) {
            throw std::runtime_error("Failed to create texture descriptor set layout");
        }
    }

    // One pipeline layout shared by every graphics pipeline
    VkDescriptorSetLayout setLayouts[3] = { vertexBufferSetLayout, fragmentBufferSetLayout, textureSetLayout };
    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pushRange.offset = 0;
    pushRange.size = 128;  // Vulkan guaranteed minimum

    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 3;
    layoutInfo.pSetLayouts = setLayouts;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushRange;
    if (vkCreatePipelineLayout(device, &layoutInfo, nullptr, &globalPipelineLayout) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create global pipeline layout");
    }

    // Compute set layouts + layout (same model, compute stage)
    {
        VkDescriptorSetLayoutBinding bindings[BINDINGS_PER_SET];
        for (Uint32 i = 0; i < BINDINGS_PER_SET; i++) {
            bindings[i] = {};
            bindings[i].binding = i;
            bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            bindings[i].descriptorCount = 1;
            bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        }
        VkDescriptorSetLayoutCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        info.bindingCount = BINDINGS_PER_SET;
        info.pBindings = bindings;
        if (vkCreateDescriptorSetLayout(device, &info, nullptr, &computeBufferSetLayout) != VK_SUCCESS) {
            throw std::runtime_error("Failed to create compute buffer set layout");
        }
        for (Uint32 i = 0; i < BINDINGS_PER_SET; i++) {
            bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        }
        if (vkCreateDescriptorSetLayout(device, &info, nullptr, &computeImageSetLayout) != VK_SUCCESS) {
            throw std::runtime_error("Failed to create compute image set layout");
        }

        VkDescriptorSetLayout computeSets[2] = { computeBufferSetLayout, computeImageSetLayout };
        VkPushConstantRange computePush{};
        computePush.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        computePush.offset = 0;
        computePush.size = 128;
        VkPipelineLayoutCreateInfo computeLayoutInfo{};
        computeLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        computeLayoutInfo.setLayoutCount = 2;
        computeLayoutInfo.pSetLayouts = computeSets;
        computeLayoutInfo.pushConstantRangeCount = 1;
        computeLayoutInfo.pPushConstantRanges = &computePush;
        if (vkCreatePipelineLayout(device, &computeLayoutInfo, nullptr, &computePipelineLayout) != VK_SUCCESS) {
            throw std::runtime_error("Failed to create compute pipeline layout");
        }
    }

    // Per-frame descriptor pools, reset wholesale each frame
    descriptorPools.resize(MAX_FRAMES_IN_FLIGHT);
    for (auto& pool : descriptorPools) {
        VkDescriptorPoolSize sizes[3] = {
            { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 8192 },
            { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4096 },
            { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1024 },
        };
        VkDescriptorPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.maxSets = 4096;
        poolInfo.poolSizeCount = 3;
        poolInfo.pPoolSizes = sizes;
        if (vkCreateDescriptorPool(device, &poolInfo, nullptr, &pool) != VK_SUCCESS) {
            throw std::runtime_error("Failed to create descriptor pool");
        }
    }
}

void RHI_Vulkan::destroyDescriptorInfrastructure() {
    for (auto& pool : descriptorPools) {
        if (pool != VK_NULL_HANDLE) vkDestroyDescriptorPool(device, pool, nullptr);
    }
    descriptorPools.clear();
    if (globalPipelineLayout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(device, globalPipelineLayout, nullptr);
        globalPipelineLayout = VK_NULL_HANDLE;
    }
    if (computePipelineLayout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(device, computePipelineLayout, nullptr);
        computePipelineLayout = VK_NULL_HANDLE;
    }
    for (VkDescriptorSetLayout* layout : { &vertexBufferSetLayout, &fragmentBufferSetLayout, &textureSetLayout,
                                           &computeBufferSetLayout, &computeImageSetLayout }) {
        if (*layout != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(device, *layout, nullptr);
            *layout = VK_NULL_HANDLE;
        }
    }
}

void RHI_Vulkan::flushDescriptors() {
    if (!descriptorsDirty || currentCommandBuffer == VK_NULL_HANDLE) {
        return;
    }
    descriptorsDirty = false;

    VkDescriptorSetLayout layouts[3] = { vertexBufferSetLayout, fragmentBufferSetLayout, textureSetLayout };
    VkDescriptorSet sets[3];
    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = descriptorPools[currentFrameInFlight];
    allocInfo.descriptorSetCount = 3;
    allocInfo.pSetLayouts = layouts;
    if (vkAllocateDescriptorSets(device, &allocInfo, sets) != VK_SUCCESS) {
        fmt::print(stderr, "flushDescriptors: descriptor pool exhausted\n");
        return;
    }

    // Write only the slots that have been bound; untouched slots stay
    // undefined, which is fine as long as no bound shader statically uses them.
    VkWriteDescriptorSet writes[BINDINGS_PER_SET * 3];
    VkDescriptorBufferInfo bufferInfos[BINDINGS_PER_SET * 2];
    VkDescriptorImageInfo imageInfos[BINDINGS_PER_SET];
    Uint32 writeCount = 0, bufferCount = 0, imageCount = 0;

    auto writeBuffers = [&](const BufferBinding* bindings, VkDescriptorSet set) {
        for (Uint32 i = 0; i < BINDINGS_PER_SET; i++) {
            if (bindings[i].buffer == VK_NULL_HANDLE) continue;
            VkDescriptorBufferInfo& info = bufferInfos[bufferCount++];
            info = { bindings[i].buffer, bindings[i].offset, bindings[i].range };
            VkWriteDescriptorSet& w = writes[writeCount++];
            w = {};
            w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w.dstSet = set;
            w.dstBinding = i;
            w.descriptorCount = 1;
            w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            w.pBufferInfo = &info;
        }
    };
    writeBuffers(boundVertexBuffers, sets[0]);
    writeBuffers(boundFragmentBuffers, sets[1]);

    for (Uint32 i = 0; i < BINDINGS_PER_SET; i++) {
        if (boundTextures[i].view == VK_NULL_HANDLE) continue;
        VkDescriptorImageInfo& info = imageInfos[imageCount++];
        info = { boundTextures[i].sampler, boundTextures[i].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkWriteDescriptorSet& w = writes[writeCount++];
        w = {};
        w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w.dstSet = sets[2];
        w.dstBinding = i;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w.pImageInfo = &info;
    }

    if (writeCount > 0) {
        vkUpdateDescriptorSets(device, writeCount, writes, 0, nullptr);
    }
    vkCmdBindDescriptorSets(currentCommandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            globalPipelineLayout, 0, 3, sets, 0, nullptr);
}

void RHI_Vulkan::transitionImage(VkImage image, VkImageLayout from, VkImageLayout to, VkImageAspectFlags aspect) {
    if (from == to || currentCommandBuffer == VK_NULL_HANDLE) {
        return;
    }
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = from;
    barrier.newLayout = to;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = aspect;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = VK_REMAINING_MIP_LEVELS;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = VK_REMAINING_ARRAY_LAYERS;
    // Conservative full barrier — correctness first; refine when the render
    // graph learns resource dependencies.
    barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT;
    barrier.dstAccessMask = VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT;
    vkCmdPipelineBarrier(currentCommandBuffer,
                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &barrier);
}

void RHI_Vulkan::shutdown() {
    // Guard against double shutdown (explicit shutdown() + destructor)
    if (device == VK_NULL_HANDLE && instance == VK_NULL_HANDLE) {
        return;
    }
    if (device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device);

        // Flush any pending uploads FIRST, while the textures/buffers they copy
        // into are still alive. A failed initialize() (or any teardown before a
        // frame completes) can leave an unsubmitted upload command buffer with
        // buffer->image copies recorded; submitting it after the images are
        // destroyed copies into freed memory — tolerated by lavapipe but a hard
        // crash on MoltenVK. Reclaim deferred/staging resources here too.
        submitUploads(true);
        processRetirements(true);
        destroyUploadStream();
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
        // Lazily-created per-array-layer views (render-to-layer) must be freed
        // too, or they leak past vkDestroyDevice.
        for (auto& [layer, lv] : texture.layerViews) {
            if (lv != VK_NULL_HANDLE) vkDestroyImageView(device, lv, nullptr);
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
        // pipeline.layout is the shared globalPipelineLayout, destroyed exactly
        // once in destroyDescriptorInfrastructure() — destroying it per pipeline
        // here would free the same handle N times (Invalid VkPipelineLayout).
    }
    pipelines.clear();

    // Uploads/retirements/staging were already flushed right after waitIdle
    // (before resource destruction), so nothing to do for them here.
    for (auto& pool : timestampPools) {
        if (pool != VK_NULL_HANDLE) vkDestroyQueryPool(device, pool, nullptr);
    }
    timestampPools.clear();
    slotTimestamps.clear();

    // Destroy compute pipelines
    for (auto& [id, pipeline] : computePipelines) {
        if (pipeline.pipeline != VK_NULL_HANDLE) {
            vkDestroyPipeline(device, pipeline.pipeline, nullptr);
        }
        // pipeline.layout is the shared computePipelineLayout, destroyed exactly
        // once in destroyDescriptorInfrastructure() — see the graphics loop above.
    }
    computePipelines.clear();

    destroyDescriptorInfrastructure();

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

    // Null out everything so a second shutdown() (e.g. from the destructor
    // after an explicit shutdown) is a clean no-op instead of double-frees.
    imageAvailableSemaphores.clear();
    renderFinishedSemaphores.clear();
    inFlightFences.clear();
    commandBuffers.clear();
    swapchainImageViews.clear();
    swapchainImages.clear();
    commandPool = VK_NULL_HANDLE;
    swapchain = VK_NULL_HANDLE;
    currentCommandBuffer = VK_NULL_HANDLE;
    device = VK_NULL_HANDLE;
    surface = VK_NULL_HANDLE;
    instance = VK_NULL_HANDLE;
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
    const bool hostVisible = desc.memoryUsage != MemoryUsage::GPU;
    buffers[id] = {buffer, memory, desc.size, false, nullptr, hostVisible};

    return BufferHandle{id};
}

void RHI_Vulkan::destroyBuffer(BufferHandle handle) {
    auto it = buffers.find(handle.id);
    if (it != buffers.end()) {
        // The handle dies now; the Vulkan objects retire once every frame
        // that could still reference them has completed.
        VkDevice dev = device;
        VkBuffer buf = it->second.buffer;
        VkDeviceMemory mem = it->second.memory;
        deferDestroy([dev, buf, mem]() {
            if (buf != VK_NULL_HANDLE) vkDestroyBuffer(dev, buf, nullptr);
            if (mem != VK_NULL_HANDLE) vkFreeMemory(dev, mem, nullptr);
        });
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
    if (desc.isCube) {
        imageInfo.flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    }
    imageInfo.extent.width = desc.width;
    imageInfo.extent.height = desc.height;
    imageInfo.extent.depth = desc.depth;
    imageInfo.mipLevels = desc.mipLevels;
    imageInfo.arrayLayers = desc.arrayLayers;
    imageInfo.format = convertPixelFormat(desc.format);
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage = convertTextureUsage(desc.usage);
    imageInfo.samples = static_cast<VkSampleCountFlagBits>(desc.sampleCount);
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
    viewInfo.viewType = desc.isCube ? VK_IMAGE_VIEW_TYPE_CUBE
                       : desc.arrayLayers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY
                                              : VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = convertPixelFormat(desc.format);
    // Depth formats need the depth aspect, not color
    const bool isDepthFormat =
        desc.format == PixelFormat::Depth32Float || desc.format == PixelFormat::Depth24Stencil8;
    viewInfo.subresourceRange.aspectMask =
        isDepthFormat ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
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
    TextureResource resource{};
    resource.image = image;
    resource.view = view;
    resource.memory = memory;
    resource.format = convertPixelFormat(desc.format);
    resource.width = desc.width;
    resource.height = desc.height;
    resource.arrayLayers = desc.arrayLayers;
    resource.usage = imageInfo.usage;
    resource.currentLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    textures[id] = resource;

    return TextureHandle{id};
}

void RHI_Vulkan::destroyTexture(TextureHandle handle) {
    auto it = textures.find(handle.id);
    if (it != textures.end()) {
        VkDevice dev = device;
        VkImageView view = it->second.view;
        VkImage image = it->second.image;
        VkDeviceMemory mem = it->second.memory;
        std::vector<VkImageView> extraViews;
        for (auto& lv : it->second.layerViews) extraViews.push_back(lv.second);
        deferDestroy([dev, view, image, mem, extraViews]() {
            if (view != VK_NULL_HANDLE) vkDestroyImageView(dev, view, nullptr);
            for (VkImageView lv : extraViews) {
                if (lv != VK_NULL_HANDLE) vkDestroyImageView(dev, lv, nullptr);
            }
            if (image != VK_NULL_HANDLE) vkDestroyImage(dev, image, nullptr);
            if (mem != VK_NULL_HANDLE) vkFreeMemory(dev, mem, nullptr);
        });
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
        VkDevice dev = device;
        VkSampler s = it->second.sampler;
        deferDestroy([dev, s]() {
            if (s != VK_NULL_HANDLE) vkDestroySampler(dev, s, nullptr);
        });
        samplers.erase(it);
    }
}

// ============================================================================
// Resource Creation - Pipeline
// ============================================================================

PipelineHandle RHI_Vulkan::createPipeline(const PipelineDesc& desc) {
    // Get shader modules
    auto vsIt = shaders.find(desc.vertexShader.id);
    auto fsIt = shaders.find(desc.fragmentShader.id);
    if (vsIt == shaders.end() || fsIt == shaders.end()) {
        throw std::runtime_error("Invalid shader handles for pipeline");
    }

    // Shader stages
    VkPipelineShaderStageCreateInfo vertShaderStageInfo{};
    vertShaderStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    vertShaderStageInfo.stage = VK_SHADER_STAGE_VERTEX_BIT;
    vertShaderStageInfo.module = vsIt->second.module;
    vertShaderStageInfo.pName = "main";

    VkPipelineShaderStageCreateInfo fragShaderStageInfo{};
    fragShaderStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    fragShaderStageInfo.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    fragShaderStageInfo.module = fsIt->second.module;
    fragShaderStageInfo.pName = "main";

    VkPipelineShaderStageCreateInfo shaderStages[] = {vertShaderStageInfo, fragShaderStageInfo};

    // Vertex input state
    std::vector<VkVertexInputAttributeDescription> attributeDescriptions;
    for (const auto& attr : desc.vertexLayout.attributes) {
        VkVertexInputAttributeDescription attrDesc{};
        attrDesc.location = attr.location;
        attrDesc.binding = 0;
        attrDesc.format = convertPixelFormat(attr.format);
        attrDesc.offset = attr.offset;
        attributeDescriptions.push_back(attrDesc);
    }

    VkVertexInputBindingDescription bindingDescription{};
    bindingDescription.binding = 0;
    bindingDescription.stride = desc.vertexLayout.stride;
    bindingDescription.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    VkPipelineVertexInputStateCreateInfo vertexInputInfo{};
    vertexInputInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInputInfo.vertexBindingDescriptionCount = 1;
    vertexInputInfo.pVertexBindingDescriptions = &bindingDescription;
    vertexInputInfo.vertexAttributeDescriptionCount = static_cast<uint32_t>(attributeDescriptions.size());
    vertexInputInfo.pVertexAttributeDescriptions = attributeDescriptions.data();

    // Input assembly
    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = convertPrimitiveTopology(desc.topology);
    inputAssembly.primitiveRestartEnable = VK_FALSE;

    // Viewport and scissor (dynamic)
    VkPipelineViewportStateCreateInfo viewportState{};
    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;

    // Rasterization
    VkPipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.depthClampEnable = VK_FALSE;
    rasterizer.rasterizerDiscardEnable = VK_FALSE;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.lineWidth = 1.0f;
    rasterizer.cullMode = convertCullMode(desc.cullMode);
    rasterizer.frontFace = desc.frontFaceCounterClockwise ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;
    rasterizer.depthBiasEnable = VK_FALSE;

    // Multisampling
    VkPipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.sampleShadingEnable = VK_FALSE;
    multisampling.rasterizationSamples = static_cast<VkSampleCountFlagBits>(desc.sampleCount);

    // Depth and stencil
    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable = desc.depthTest ? VK_TRUE : VK_FALSE;
    depthStencil.depthWriteEnable = desc.depthWrite ? VK_TRUE : VK_FALSE;
    depthStencil.depthCompareOp = convertCompareOp(desc.depthCompareOp);
    depthStencil.depthBoundsTestEnable = VK_FALSE;
    depthStencil.stencilTestEnable = VK_FALSE;

    // Color blending
    VkPipelineColorBlendAttachmentState colorBlendAttachment{};
    colorBlendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                          VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    switch (desc.blendMode) {
        case BlendMode::Opaque:
            colorBlendAttachment.blendEnable = VK_FALSE;
            break;
        case BlendMode::AlphaBlend:
            colorBlendAttachment.blendEnable = VK_TRUE;
            colorBlendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
            colorBlendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            colorBlendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
            colorBlendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            colorBlendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
            colorBlendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;
            break;
        case BlendMode::Additive:
            colorBlendAttachment.blendEnable = VK_TRUE;
            colorBlendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
            colorBlendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
            colorBlendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
            colorBlendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            colorBlendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            colorBlendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;
            break;
        case BlendMode::Multiply:
            colorBlendAttachment.blendEnable = VK_TRUE;
            colorBlendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_DST_COLOR;
            colorBlendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ZERO;
            colorBlendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
            colorBlendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            colorBlendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
            colorBlendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;
            break;
    }

    // Same blend state replicated across all color attachments
    std::vector<VkPipelineColorBlendAttachmentState> blendAttachments(
        std::max<size_t>(1, desc.colorAttachmentFormats.size()), colorBlendAttachment);
    VkPipelineColorBlendStateCreateInfo colorBlending{};
    colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlending.logicOpEnable = VK_FALSE;
    colorBlending.logicOp = VK_LOGIC_OP_COPY;
    colorBlending.attachmentCount = static_cast<uint32_t>(blendAttachments.size());
    colorBlending.pAttachments = blendAttachments.data();

    // Dynamic states
    std::vector<VkDynamicState> dynamicStates = {
        VK_DYNAMIC_STATE_VIEWPORT,
        VK_DYNAMIC_STATE_SCISSOR
    };

    VkPipelineDynamicStateCreateInfo dynamicState{};
    dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = static_cast<uint32_t>(dynamicStates.size());
    dynamicState.pDynamicStates = dynamicStates.data();

    // All graphics pipelines share the global layout (see descriptor binding
    // model in rhi_vulkan.hpp)
    VkPipelineLayout pipelineLayout = globalPipelineLayout;

    // Dynamic rendering info: attachment formats are baked into the pipeline
    // and must match the render pass (PixelFormat::Swapchain resolves here)
    std::vector<VkFormat> colorFormats;
    colorFormats.reserve(desc.colorAttachmentFormats.size());
    for (PixelFormat f : desc.colorAttachmentFormats) {
        colorFormats.push_back(f == PixelFormat::Swapchain ? swapchainImageFormat
                                                           : convertPixelFormat(f));
    }
    VkPipelineRenderingCreateInfo pipelineRenderingInfo{};
    pipelineRenderingInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    pipelineRenderingInfo.colorAttachmentCount = static_cast<uint32_t>(colorFormats.size());
    pipelineRenderingInfo.pColorAttachmentFormats = colorFormats.data();
    if (desc.hasDepthAttachment) {
        pipelineRenderingInfo.depthAttachmentFormat = convertPixelFormat(desc.depthAttachmentFormat);
    }

    // Create graphics pipeline
    VkGraphicsPipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.pNext = &pipelineRenderingInfo;
    pipelineInfo.stageCount = 2;
    pipelineInfo.pStages = shaderStages;
    pipelineInfo.pVertexInputState = &vertexInputInfo;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState = &viewportState;
    pipelineInfo.pRasterizationState = &rasterizer;
    pipelineInfo.pMultisampleState = &multisampling;
    pipelineInfo.pDepthStencilState = &depthStencil;
    pipelineInfo.pColorBlendState = &colorBlending;
    pipelineInfo.pDynamicState = &dynamicState;
    pipelineInfo.layout = pipelineLayout;
    pipelineInfo.renderPass = VK_NULL_HANDLE; // Using dynamic rendering
    pipelineInfo.subpass = 0;
    pipelineInfo.basePipelineHandle = VK_NULL_HANDLE;

    VkPipeline pipeline;
    if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create graphics pipeline");
    }

    Uint32 id = nextPipelineId++;
    pipelines[id] = {pipeline, pipelineLayout};

    return PipelineHandle{id};
}

void RHI_Vulkan::destroyPipeline(PipelineHandle handle) {
    auto it = pipelines.find(handle.id);
    if (it != pipelines.end()) {
        VkDevice dev = device;
        VkPipeline pl = it->second.pipeline;
        deferDestroy([dev, pl]() {
            if (pl != VK_NULL_HANDLE) vkDestroyPipeline(dev, pl, nullptr);
        });
        // layout is the shared global pipeline layout — not owned per pipeline
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

    if (it->second.hostVisible) {
        void* mapped;
        vkMapMemory(device, it->second.memory, offset, size, 0, &mapped);
        std::memcpy(mapped, data, size);
        vkUnmapMemory(device, it->second.memory);
        return;
    }

    // DEVICE_LOCAL buffer: copy through the staging ring into the batched
    // upload command stream (submitted at beginFrame / flushUploads() / wrap).
    VkDeviceSize srcOffset;
    VkBuffer srcBuf = stageData(data, size, srcOffset);
    VkBufferCopy region{ srcOffset, offset, size };
    vkCmdCopyBuffer(ensureUploadCmd(), srcBuf, it->second.buffer, 1, &region);
}

void RHI_Vulkan::updateTexture(TextureHandle handle, const void* data, size_t size,
                               Uint32 mipLevel, Uint32 arrayLayer) {
    auto it = textures.find(handle.id);
    if (it == textures.end()) {
        return;
    }
    TextureResource& tex = it->second;

    Uint32 mipWidth = std::max(1u, tex.width >> mipLevel);
    Uint32 mipHeight = std::max(1u, tex.height >> mipLevel);

    // Stage through the ring (or a dedicated buffer when oversize) into the
    // batched upload stream
    VkDeviceSize srcOffset;
    VkBuffer srcBuffer = stageData(data, size, srcOffset);

    VkCommandBuffer cmd = ensureUploadCmd();

    // First touch of a subresource: whole-image transition to TRANSFER_DST.
    // (Layout is tracked per image, not per mip/layer — uploads are expected
    // to happen before the texture is sampled.)
    if (tex.currentLayout != VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
        VkImageMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.oldLayout = tex.currentLayout;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = tex.image;
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.baseMipLevel = 0;
        barrier.subresourceRange.levelCount = VK_REMAINING_MIP_LEVELS;
        barrier.subresourceRange.baseArrayLayer = 0;
        barrier.subresourceRange.layerCount = VK_REMAINING_ARRAY_LAYERS;
        barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &barrier);
        tex.currentLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    }

    VkBufferImageCopy region{};
    region.bufferOffset = srcOffset;
    region.bufferRowLength = 0;   // tightly packed
    region.bufferImageHeight = 0;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel = mipLevel;
    region.imageSubresource.baseArrayLayer = arrayLayer;
    region.imageSubresource.layerCount = 1;
    region.imageOffset = {0, 0, 0};
    region.imageExtent = {mipWidth, mipHeight, 1};
    vkCmdCopyBufferToImage(cmd, srcBuffer, tex.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    // Leave the image shader-readable; further uploads transition back
    VkImageMemoryBarrier toRead{};
    toRead.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toRead.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toRead.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    toRead.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toRead.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toRead.image = tex.image;
    toRead.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    toRead.subresourceRange.baseMipLevel = 0;
    toRead.subresourceRange.levelCount = VK_REMAINING_MIP_LEVELS;
    toRead.subresourceRange.baseArrayLayer = 0;
    toRead.subresourceRange.layerCount = VK_REMAINING_ARRAY_LAYERS;
    toRead.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toRead.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &toRead);
    tex.currentLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
}

void RHI_Vulkan::generateMipmaps(TextureHandle handle) {
    auto it = textures.find(handle.id);
    if (it == textures.end()) {
        return;
    }
    TextureResource& tex = it->second;

    // Mip level count comes from the image; recompute from dimensions
    Uint32 mipLevels = 1;
    for (Uint32 d = std::max(tex.width, tex.height); d > 1; d >>= 1) mipLevels++;
    if (mipLevels <= 1) {
        return;
    }

    VkCommandBuffer cmd = ensureUploadCmd();

    auto subresourceBarrier = [&](Uint32 baseMip, Uint32 levelCount,
                                  VkImageLayout from, VkImageLayout to,
                                  VkAccessFlags srcAccess, VkAccessFlags dstAccess) {
        VkImageMemoryBarrier b{};
        b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.oldLayout = from;
        b.newLayout = to;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = tex.image;
        b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        b.subresourceRange.baseMipLevel = baseMip;
        b.subresourceRange.levelCount = levelCount;
        b.subresourceRange.baseArrayLayer = 0;
        b.subresourceRange.layerCount = VK_REMAINING_ARRAY_LAYERS;
        b.srcAccessMask = srcAccess;
        b.dstAccessMask = dstAccess;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &b);
    };

    // Whole image -> TRANSFER_DST as the baseline
    subresourceBarrier(0, VK_REMAINING_MIP_LEVELS, tex.currentLayout,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT,
                       VK_ACCESS_TRANSFER_WRITE_BIT);

    int32_t srcW = static_cast<int32_t>(tex.width);
    int32_t srcH = static_cast<int32_t>(tex.height);
    for (Uint32 mip = 1; mip < mipLevels; mip++) {
        // Source level: DST -> SRC
        subresourceBarrier(mip - 1, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);

        int32_t dstW = std::max(1, srcW / 2);
        int32_t dstH = std::max(1, srcH / 2);
        VkImageBlit blit{};
        blit.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, mip - 1, 0, tex.arrayLayers };
        blit.srcOffsets[1] = { srcW, srcH, 1 };
        blit.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, mip, 0, tex.arrayLayers };
        blit.dstOffsets[1] = { dstW, dstH, 1 };
        vkCmdBlitImage(cmd, tex.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       tex.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       1, &blit, VK_FILTER_LINEAR);
        srcW = dstW;
        srcH = dstH;
    }

    // All levels -> SHADER_READ (levels 0..N-2 are in SRC, last is in DST)
    subresourceBarrier(0, mipLevels - 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                       VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT);
    subresourceBarrier(mipLevels - 1, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                       VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
    tex.currentLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
}

BufferHandle RHI_Vulkan::copySwapchainToBuffer(Uint32& outWidth, Uint32& outHeight) {
    if (currentCommandBuffer == VK_NULL_HANDLE) {
        return BufferHandle{};  // frame was skipped
    }

    outWidth = swapchainExtent.width;
    outHeight = swapchainExtent.height;
    VkDeviceSize imageSize = outWidth * outHeight * 4; // RGBA8

    // Create staging buffer with HOST_VISIBLE memory for CPU readback
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = imageSize;
    bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VkBuffer stagingBuffer;
    if (vkCreateBuffer(device, &bufferInfo, nullptr, &stagingBuffer) != VK_SUCCESS) {
        fmt::print(stderr, "Failed to create screenshot staging buffer\n");
        return BufferHandle{};
    }

    VkMemoryRequirements memRequirements;
    vkGetBufferMemoryRequirements(device, stagingBuffer, &memRequirements);

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memRequirements.size;
    allocInfo.memoryTypeIndex = findMemoryType(memRequirements.memoryTypeBits,
                                                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

    VkDeviceMemory stagingMemory;
    if (vkAllocateMemory(device, &allocInfo, nullptr, &stagingMemory) != VK_SUCCESS) {
        vkDestroyBuffer(device, stagingBuffer, nullptr);
        fmt::print(stderr, "Failed to allocate screenshot staging buffer memory\n");
        return BufferHandle{};
    }

    vkBindBufferMemory(device, stagingBuffer, stagingMemory, 0);

    // Transition swapchain image for transfer (from whatever layout the
    // frame's passes left it in — usually COLOR_ATTACHMENT_OPTIMAL, since
    // screenshots are captured before endFrame's present transition)
    transitionImage(swapchainImages[currentSwapchainImageIndex],
                    swapchainImageLayout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_IMAGE_ASPECT_COLOR_BIT);

    // Copy image to buffer
    VkBufferImageCopy region{};
    region.bufferOffset = 0;
    region.bufferRowLength = 0;
    region.bufferImageHeight = 0;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = 1;
    region.imageOffset = {0, 0, 0};
    region.imageExtent = {outWidth, outHeight, 1};

    vkCmdCopyImageToBuffer(
        currentCommandBuffer,
        swapchainImages[currentSwapchainImageIndex],
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        stagingBuffer,
        1,
        &region
    );

    // Restore the pre-copy layout so endFrame's present transition stays valid
    transitionImage(swapchainImages[currentSwapchainImageIndex],
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, swapchainImageLayout,
                    VK_IMAGE_ASPECT_COLOR_BIT);

    // Store in buffers map
    Uint32 id = nextBufferId++;
    buffers[id] = {stagingBuffer, stagingMemory, imageSize, false, nullptr, true};

    return BufferHandle{id};
}

void* RHI_Vulkan::mapBuffer(BufferHandle handle) {
    auto it = buffers.find(handle.id);
    if (it == buffers.end()) {
        return nullptr;
    }

    BufferResource& bufferRes = it->second;
    if (bufferRes.isMapped) {
        return bufferRes.mappedData;
    }

    void* data = nullptr;
    if (vkMapMemory(device, bufferRes.memory, 0, bufferRes.size, 0, &data) == VK_SUCCESS) {
        bufferRes.isMapped = true;
        bufferRes.mappedData = data;
        return data;
    }

    return nullptr;
}

void RHI_Vulkan::unmapBuffer(BufferHandle handle) {
    auto it = buffers.find(handle.id);
    if (it == buffers.end() || !it->second.isMapped) {
        return;
    }

    vkUnmapMemory(device, it->second.memory);
    it->second.isMapped = false;
    it->second.mappedData = nullptr;
}

// ============================================================================
// Frame Operations
// ============================================================================

void RHI_Vulkan::beginFrame() {
    // Null until a frame is successfully begun; every command-recording call
    // and endFrame() no-op while it stays null (skipped frame).
    currentCommandBuffer = VK_NULL_HANDLE;

    // Submit any pending uploads first: same-queue submission order makes the
    // data visible to this frame's commands without a CPU wait.
    submitUploads(false);

    vkWaitForFences(device, 1, &inFlightFences[currentFrameInFlight], VK_TRUE, UINT64_MAX);

    // This slot's previous frame has now provably completed
    processRetirements(false);

    // Reap completed upload submissions; when none remain in flight the
    // staging ring can rewind to the start.
    for (size_t i = 0; i < pendingUploadFences.size();) {
        if (vkGetFenceStatus(device, pendingUploadFences[i]) == VK_SUCCESS) {
            vkDestroyFence(device, pendingUploadFences[i], nullptr);
            pendingUploadFences.erase(pendingUploadFences.begin() + i);
        } else {
            ++i;
        }
    }
    if (pendingUploadFences.empty() && uploadCmd == VK_NULL_HANDLE) {
        stagingRingOffset = 0;
    }
    if (gpuTimingSupported) {
        collectTimestamps(currentFrameInFlight);
    }

    // Deferred swapchain recreation (resize / OUT_OF_DATE / SUBOPTIMAL flagged
    // by a previous acquire or present). Done before this frame's acquire so
    // the whole frame renders at the new size.
    if (swapchainDirty) {
        recreateSwapchain();
        swapchainDirty = false;
    }

    VkResult result = vkAcquireNextImageKHR(device, swapchain, UINT64_MAX,
                                            imageAvailableSemaphores[currentFrameInFlight],
                                            VK_NULL_HANDLE, &currentSwapchainImageIndex);

    if (result == VK_ERROR_OUT_OF_DATE_KHR) {
        // Unusable swapchain: recreate next frame and skip this one. The fence
        // was not reset, so the next beginFrame() passes the wait immediately.
        swapchainDirty = true;
        return;
    }
    if (result == VK_SUBOPTIMAL_KHR) {
        // Acquire succeeded — render this frame, refresh the swapchain next.
        swapchainDirty = true;
    }

    vkResetFences(device, 1, &inFlightFences[currentFrameInFlight]);

    currentCommandBuffer = commandBuffers[currentFrameInFlight];
    vkResetCommandBuffer(currentCommandBuffer, 0);

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    vkBeginCommandBuffer(currentCommandBuffer, &beginInfo);

    // Reclaim this frame slot's descriptor sets (GPU finished with them — the
    // fence wait above proves it) and force a fresh set on the first draw.
    if (!descriptorPools.empty()) {
        vkResetDescriptorPool(device, descriptorPools[currentFrameInFlight], 0);
    }
    descriptorsDirty = true;
    computeDescriptorsDirty = true;

    // GPU pass timing: reset this slot's query pool and latch the enable flag
    gpuTimingActiveThisFrame = gpuTimingEnabled && gpuTimingSupported;
    nextTimestampQuery = 0;
    if (gpuTimingSupported) {
        vkCmdResetQueryPool(currentCommandBuffer, timestampPools[currentFrameInFlight],
                            0, TIMESTAMP_QUERIES_PER_POOL);
    }

    // The acquired swapchain image's previous content is irrelevant (the
    // first pass clears); treat its layout as undefined for transitions.
    swapchainImageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
}

void RHI_Vulkan::endFrame() {
    // Frame was skipped (e.g. out-of-date swapchain in beginFrame)
    if (currentCommandBuffer == VK_NULL_HANDLE) {
        return;
    }

    // Swapchain image must be in PRESENT_SRC layout for vkQueuePresentKHR
    transitionImage(swapchainImages[currentSwapchainImageIndex],
                    swapchainImageLayout, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_ASPECT_COLOR_BIT);
    swapchainImageLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

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

    VkSemaphore signalSemaphores[] = {renderFinishedSemaphores[currentSwapchainImageIndex]};
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

    VkResult presentResult = vkQueuePresentKHR(presentQueue, &presentInfo);
    if (presentResult == VK_ERROR_OUT_OF_DATE_KHR || presentResult == VK_SUBOPTIMAL_KHR) {
        swapchainDirty = true;  // recreate at the top of the next beginFrame
    }

    frameCounter++;  // retirement clock: this frame's work is now "in flight"
    currentFrameInFlight = (currentFrameInFlight + 1) % MAX_FRAMES_IN_FLIGHT;
}

VkImageView RHI_Vulkan::getDepthLayerView(TextureResource& tex, Uint32 layer) {
    return getSubresourceView(tex, layer, 0, VK_IMAGE_ASPECT_DEPTH_BIT);
}

// Lazily-created single-layer/mip views for render-to-layer passes (cascaded
// shadows, cube-face IBL capture, prefilter mip chains). Keyed so depth and
// color views of the same texture never collide.
VkImageView RHI_Vulkan::getSubresourceView(TextureResource& tex, Uint32 layer, Uint32 mip,
                                           VkImageAspectFlags aspect) {
    Uint32 key = (mip << 20) | ((layer + 1) << 1) | (aspect == VK_IMAGE_ASPECT_DEPTH_BIT ? 1u : 0u);
    auto found = tex.layerViews.find(key);
    if (found != tex.layerViews.end()) return found->second;

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = tex.image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = tex.format;
    viewInfo.subresourceRange.aspectMask = aspect;
    viewInfo.subresourceRange.baseMipLevel = mip;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.baseArrayLayer = layer;
    viewInfo.subresourceRange.layerCount = 1;

    VkImageView view = VK_NULL_HANDLE;
    if (vkCreateImageView(device, &viewInfo, nullptr, &view) != VK_SUCCESS) {
        return VK_NULL_HANDLE;
    }
    tex.layerViews[key] = view;
    return view;
}

void RHI_Vulkan::beginRenderPass(const RenderPassDesc& desc) {
    if (currentCommandBuffer == VK_NULL_HANDLE) {
        return;  // frame was skipped
    }

    currentPassColorTextures.clear();
    currentPassDepthTexture = 0;

    // Setup color attachments
    std::vector<VkRenderingAttachmentInfo> colorAttachments;
    for (size_t i = 0; i < desc.colorAttachments.size(); ++i) {
        const auto& colorAttachmentHandle = desc.colorAttachments[i];
        VkRenderingAttachmentInfo attachmentInfo{};
        attachmentInfo.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;

        // Get the image view for this attachment (+ transition to attachment layout)
        if (colorAttachmentHandle.id == 0) {
            // Use swapchain image
            attachmentInfo.imageView = swapchainImageViews[currentSwapchainImageIndex];
            transitionImage(swapchainImages[currentSwapchainImageIndex],
                            swapchainImageLayout, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                            VK_IMAGE_ASPECT_COLOR_BIT);
            swapchainImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        } else {
            auto it = textures.find(colorAttachmentHandle.id);
            if (it != textures.end()) {
                // Attachment 0 may target a single cube face / array layer and
                // mip (IBL capture, prefilter chains).
                if (i == 0 && (desc.colorArrayLayer != ~0u || desc.colorMipLevel != 0)) {
                    Uint32 layer = (desc.colorArrayLayer != ~0u) ? desc.colorArrayLayer : 0;
                    attachmentInfo.imageView =
                        getSubresourceView(it->second, layer, desc.colorMipLevel, VK_IMAGE_ASPECT_COLOR_BIT);
                } else {
                    attachmentInfo.imageView = it->second.view;
                }
                transitionImage(it->second.image, it->second.currentLayout,
                                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
                it->second.currentLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                currentPassColorTextures.push_back(colorAttachmentHandle.id);
            }
        }

        attachmentInfo.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        // Load/store operations from loadColor vector (true = load, false = clear)
        bool shouldLoad = (i < desc.loadColor.size()) ? desc.loadColor[i] : false;
        attachmentInfo.loadOp = shouldLoad ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_CLEAR;
        attachmentInfo.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

        // Clear color from clearColors vector
        if (i < desc.clearColors.size()) {
            const auto& clearColor = desc.clearColors[i];
            attachmentInfo.clearValue.color = {{clearColor.r, clearColor.g, clearColor.b, clearColor.a}};
        } else {
            attachmentInfo.clearValue.color = {{0.0f, 0.0f, 0.0f, 1.0f}};
        }

        colorAttachments.push_back(attachmentInfo);
    }

    // Setup depth attachment. Invalid handle = no depth (id 0 is the
    // swapchain sentinel and never a depth texture).
    VkRenderingAttachmentInfo depthAttachment{};
    const bool hasDepth = desc.depthAttachment.isValid() && desc.depthAttachment.id != 0;
    if (hasDepth) {
        depthAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        auto it = textures.find(desc.depthAttachment.id);
        if (it != textures.end()) {
            // Render into a single array layer (cascaded shadow maps, cube
            // faces, ...) when requested; otherwise the whole-texture view.
            if (desc.depthArrayLayer != ~0u && it->second.arrayLayers > 1) {
                depthAttachment.imageView = getDepthLayerView(it->second, desc.depthArrayLayer);
            } else {
                depthAttachment.imageView = it->second.view;
            }
            transitionImage(it->second.image, it->second.currentLayout,
                            VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT);
            it->second.currentLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            // Sampleable depth (shadow maps) needs a shader-read transition once
            // the pass ends; remember it for endRenderPass.
            if (it->second.usage & VK_IMAGE_USAGE_SAMPLED_BIT) {
                currentPassDepthTexture = desc.depthAttachment.id;
            }
        }
        depthAttachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        // Load/store operations (loadDepth: true = load, false = clear)
        depthAttachment.loadOp = desc.loadDepth ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_CLEAR;
        depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        depthAttachment.clearValue.depthStencil = {desc.clearDepth, desc.clearStencil};
    }

    // Begin dynamic rendering
    VkRenderingInfo renderingInfo{};
    renderingInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    renderingInfo.renderArea.offset = {0, 0};
    // Render area must match the actual attachment size — NOT the swapchain.
    // Offscreen targets can be smaller (e.g. a 512x512 render texture);
    // rendering outside the image is undefined behavior, and on software
    // rasterizers (lavapipe) it literally scribbles over the heap.
    VkExtent2D passExtent = swapchainExtent;
    if (!desc.colorAttachments.empty() && desc.colorAttachments[0].id != 0) {
        auto it = textures.find(desc.colorAttachments[0].id);
        if (it != textures.end()) {
            passExtent = { it->second.width, it->second.height };
        }
    } else if (hasDepth) {
        // Depth-only passes (e.g. shadow maps) size to the depth attachment,
        // which is usually not the swapchain resolution.
        auto it = textures.find(desc.depthAttachment.id);
        if (it != textures.end()) {
            passExtent = { it->second.width, it->second.height };
        }
    }
    renderingInfo.renderArea.extent = passExtent;
    renderingInfo.layerCount = 1;
    renderingInfo.colorAttachmentCount = static_cast<uint32_t>(colorAttachments.size());
    renderingInfo.pColorAttachments = colorAttachments.data();
    if (hasDepth) {
        renderingInfo.pDepthAttachment = &depthAttachment;
    }

    // Debug label + GPU timestamp around the pass (both no-op when disabled)
    if (desc.name) {
        beginDebugLabel(currentCommandBuffer, desc.name);
        renderPassLabelOpen = true;
    }
    Uint32 tsBegin, tsEnd;
    if (allocateTimestampPair(desc.name, tsBegin, tsEnd)) {
        vkCmdWriteTimestamp(currentCommandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                            timestampPools[currentFrameInFlight], tsBegin);
        currentPassEndQuery = tsEnd;
    } else {
        currentPassEndQuery = UINT32_MAX;
    }

    vkCmdBeginRenderingKHR(currentCommandBuffer, &renderingInfo);

    // Set viewport and scissor (attachment-sized, matching the render area).
    // Negative-height viewport (core since Vulkan 1.1): the engine's
    // projection matrices follow the GL convention (NDC +Y up) while Vulkan's
    // NDC +Y points down. Flipping here fixes both the vertical mirroring and
    // the winding order — without it, CCW front faces rasterize as CW and
    // get backface-culled.
    VkViewport viewport{};
    viewport.x = 0.0f;
    viewport.y = static_cast<float>(passExtent.height);
    viewport.width = static_cast<float>(passExtent.width);
    viewport.height = -static_cast<float>(passExtent.height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(currentCommandBuffer, 0, 1, &viewport);

    VkRect2D scissor{};
    scissor.offset = {0, 0};
    scissor.extent = passExtent;
    vkCmdSetScissor(currentCommandBuffer, 0, 1, &scissor);
}

void RHI_Vulkan::endRenderPass() {
    if (currentCommandBuffer == VK_NULL_HANDLE) {
        return;  // frame was skipped
    }
    vkCmdEndRenderingKHR(currentCommandBuffer);

    if (currentPassEndQuery != UINT32_MAX) {
        vkCmdWriteTimestamp(currentCommandBuffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                            timestampPools[currentFrameInFlight], currentPassEndQuery);
        currentPassEndQuery = UINT32_MAX;
    }
    if (renderPassLabelOpen) {
        endDebugLabel(currentCommandBuffer);
        renderPassLabelOpen = false;
    }

    // Offscreen color targets that can be sampled later (render-to-texture,
    // post-process inputs) move to shader-read layout as the pass ends.
    for (Uint32 texId : currentPassColorTextures) {
        auto it = textures.find(texId);
        if (it == textures.end()) continue;
        if (it->second.usage & VK_IMAGE_USAGE_SAMPLED_BIT) {
            transitionImage(it->second.image, it->second.currentLayout,
                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
            it->second.currentLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        }
    }
    currentPassColorTextures.clear();

    // Sampleable depth (shadow maps) -> shader-read so a later pass can sample it.
    if (currentPassDepthTexture != 0) {
        auto it = textures.find(currentPassDepthTexture);
        if (it != textures.end() && (it->second.usage & VK_IMAGE_USAGE_SAMPLED_BIT)) {
            transitionImage(it->second.image, it->second.currentLayout,
                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT);
            it->second.currentLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        }
        currentPassDepthTexture = 0;
    }
}

// ============================================================================
// Rendering Commands
// ============================================================================

void RHI_Vulkan::bindPipeline(PipelineHandle pipeline) {
    currentPipeline = pipeline;

    auto it = pipelines.find(pipeline.id);
    if (it != pipelines.end() && currentCommandBuffer != VK_NULL_HANDLE) {
        vkCmdBindPipeline(currentCommandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, it->second.pipeline);
    }
}

void RHI_Vulkan::bindVertexBuffer(BufferHandle buffer, Uint32 binding, size_t offset) {
    // The RHI's `binding` follows Metal's buffer-table semantics (e.g. the
    // main pass binds meshes at index 3 because indices 0-2 hold uniforms).
    // Vulkan separates vertex input from descriptors: pipelines declare a
    // single interleaved vertex stream at input binding 0, so bind there.
    auto it = buffers.find(buffer.id);
    if (it != buffers.end() && currentCommandBuffer != VK_NULL_HANDLE) {
        VkDeviceSize offsets[] = {offset};
        vkCmdBindVertexBuffers(currentCommandBuffer, 0, 1, &it->second.buffer, offsets);
    }
}

void RHI_Vulkan::bindIndexBuffer(BufferHandle buffer, size_t offset) {
    auto it = buffers.find(buffer.id);
    if (it != buffers.end() && currentCommandBuffer != VK_NULL_HANDLE) {
        vkCmdBindIndexBuffer(currentCommandBuffer, it->second.buffer, offset, VK_INDEX_TYPE_UINT32);
    }
}

void RHI_Vulkan::setUniformBuffer(Uint32 set, Uint32 binding, BufferHandle buffer, size_t offset, size_t range) {
    // Stage-agnostic bind (mirrors Metal's setUniformBuffer): both stages
    setVertexBuffer(binding, buffer, offset, range);
    setFragmentBuffer(binding, buffer, offset, range);
}

void RHI_Vulkan::setStorageBuffer(Uint32 set, Uint32 binding, BufferHandle buffer, size_t offset, size_t range) {
    setVertexBuffer(binding, buffer, offset, range);
    setFragmentBuffer(binding, buffer, offset, range);
}

void RHI_Vulkan::setVertexBuffer(Uint32 binding, BufferHandle buffer, size_t offset, size_t range) {
    auto it = buffers.find(buffer.id);
    if (it == buffers.end() || binding >= BINDINGS_PER_SET) {
        return;
    }
    boundVertexBuffers[binding] = { it->second.buffer, offset, range > 0 ? range : VK_WHOLE_SIZE };
    descriptorsDirty = true;
}

void RHI_Vulkan::setFragmentBuffer(Uint32 binding, BufferHandle buffer, size_t offset, size_t range) {
    auto it = buffers.find(buffer.id);
    if (it == buffers.end() || binding >= BINDINGS_PER_SET) {
        return;
    }
    boundFragmentBuffers[binding] = { it->second.buffer, offset, range > 0 ? range : VK_WHOLE_SIZE };
    descriptorsDirty = true;
}

void RHI_Vulkan::setVertexBytes(const void* data, size_t size, Uint32 binding) {
    // Vertex bytes live in push-constant region [0, 64): offset (binding%4)*16
    if (currentCommandBuffer == VK_NULL_HANDLE || !data || size == 0) {
        return;
    }
    Uint32 offset = (binding % 4) * 16;
    Uint32 clamped = static_cast<Uint32>(std::min<size_t>(size, 64 - offset));
    vkCmdPushConstants(currentCommandBuffer, globalPipelineLayout,
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                       offset, clamped, data);
}

void RHI_Vulkan::setFragmentBytes(const void* data, size_t size, Uint32 binding) {
    // Fragment bytes live in push-constant region [64, 128): 64 + (binding%4)*16
    if (currentCommandBuffer == VK_NULL_HANDLE || !data || size == 0) {
        return;
    }
    Uint32 offset = 64 + (binding % 4) * 16;
    Uint32 clamped = static_cast<Uint32>(std::min<size_t>(size, 128 - offset));
    vkCmdPushConstants(currentCommandBuffer, globalPipelineLayout,
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                       offset, clamped, data);
}

void RHI_Vulkan::setTexture(Uint32 set, Uint32 binding, TextureHandle texture, SamplerHandle sampler) {
    auto texIt = textures.find(texture.id);
    auto samplerIt = samplers.find(sampler.id);
    if (texIt == textures.end() || samplerIt == samplers.end() || binding >= BINDINGS_PER_SET) {
        return;
    }
    boundTextures[binding] = { texIt->second.view, samplerIt->second.sampler };
    descriptorsDirty = true;
}

void RHI_Vulkan::draw(Uint32 vertexCount, Uint32 instanceCount, Uint32 firstVertex, Uint32 firstInstance) {
    if (currentCommandBuffer != VK_NULL_HANDLE) {
        flushDescriptors();
        vkCmdDraw(currentCommandBuffer, vertexCount, instanceCount, firstVertex, firstInstance);
    }
}

void RHI_Vulkan::drawIndexed(Uint32 indexCount, Uint32 instanceCount, Uint32 firstIndex, int32_t vertexOffset, Uint32 firstInstance) {
    if (currentCommandBuffer != VK_NULL_HANDLE) {
        flushDescriptors();
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
    switch (swapchainImageFormat) {
        case VK_FORMAT_B8G8R8A8_SRGB: return PixelFormat::BGRA8_SRGB;
        case VK_FORMAT_B8G8R8A8_UNORM: return PixelFormat::BGRA8_UNORM;
        case VK_FORMAT_R8G8B8A8_SRGB: return PixelFormat::RGBA8_SRGB;
        case VK_FORMAT_R8G8B8A8_UNORM: return PixelFormat::RGBA8_UNORM;
        default: return PixelFormat::BGRA8_UNORM;
    }
}

// ============================================================================
// Backend Query Interface
// ============================================================================

void* RHI_Vulkan::getBackendDevice() const {
    return (void*)device;
}

void* RHI_Vulkan::getBackendTexture(TextureHandle handle) const {
    auto it = textures.find(handle.id);
    return it != textures.end() ? (void*)it->second.view : nullptr;
}

void* RHI_Vulkan::getBackendSampler(SamplerHandle handle) const {
    auto it = samplers.find(handle.id);
    return it != samplers.end() ? (void*)it->second.sampler : nullptr;
}

void* RHI_Vulkan::getBackendPhysicalDevice() const {
    return (void*)physicalDevice;
}

void* RHI_Vulkan::getBackendInstance() const {
    return (void*)instance;
}

void* RHI_Vulkan::getBackendQueue() const {
    return (void*)graphicsQueue;
}

void* RHI_Vulkan::getBackendCommandBuffer() const {
    return (void*)currentCommandBuffer;
}

// ============================================================================
// Internal Helpers - Initialization (Simplified stubs)
// ============================================================================

void RHI_Vulkan::createInstance() {
    const std::vector<const char*> validationLayers = {
#if defined(_DEBUG) || defined(DEBUG)
        "VK_LAYER_KHRONOS_validation"
#endif
    };

    std::vector<const char*> instanceExtensions = {
        VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME,
        VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME
    };

    // Debug labels (RenderDoc/validation pass names) when available
    {
        uint32_t count = 0;
        vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr);
        std::vector<VkExtensionProperties> available(count);
        vkEnumerateInstanceExtensionProperties(nullptr, &count, available.data());
        for (const auto& e : available) {
            if (std::strcmp(e.extensionName, VK_EXT_DEBUG_UTILS_EXTENSION_NAME) == 0) {
                instanceExtensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
                break;
            }
        }
    }

    uint32_t instanceExtensionCount;
    const char*const* instanceExtensionNames = SDL_Vulkan_GetInstanceExtensions(&instanceExtensionCount);
    for(uint32_t i = 0; i < instanceExtensionCount; i++) {
        instanceExtensions.emplace_back(instanceExtensionNames[i]);
    }

    const VkApplicationInfo appInfo = {
        VK_STRUCTURE_TYPE_APPLICATION_INFO,
        nullptr,
        "Project Vapor",
        VK_MAKE_VERSION(0, 1, 0),
        "No Engine",
        VK_MAKE_VERSION(0, 1, 0),
        VK_API_VERSION_1_3
    };

    const VkInstanceCreateInfo instanceInfo = {
        VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        nullptr,
        VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR,
        &appInfo,
        static_cast<uint32_t>(validationLayers.size()),
        validationLayers.data(),
        static_cast<uint32_t>(instanceExtensions.size()),
        instanceExtensions.data(),
    };

    if (vkCreateInstance(&instanceInfo, nullptr, &instance) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create Vulkan instance");
    }

    // Null when VK_EXT_debug_utils is absent — labels become no-ops
    pfnCmdBeginDebugLabel = reinterpret_cast<PFN_vkCmdBeginDebugUtilsLabelEXT>(
        vkGetInstanceProcAddr(instance, "vkCmdBeginDebugUtilsLabelEXT"));
    pfnCmdEndDebugLabel = reinterpret_cast<PFN_vkCmdEndDebugUtilsLabelEXT>(
        vkGetInstanceProcAddr(instance, "vkCmdEndDebugUtilsLabelEXT"));
}

void RHI_Vulkan::createSurface() {
    if (!SDL_Vulkan_CreateSurface(window, instance, nullptr, &surface)) {
        throw std::runtime_error("Failed to create Vulkan surface");
    }
}

void RHI_Vulkan::pickPhysicalDevice() {
    uint32_t physicalDeviceCount;
    vkEnumeratePhysicalDevices(instance, &physicalDeviceCount, nullptr);
    if (physicalDeviceCount == 0) {
        throw std::runtime_error("Failed to find any GPUs with Vulkan support");
    }

    std::vector<VkPhysicalDevice> physicalDevices(physicalDeviceCount);
    vkEnumeratePhysicalDevices(instance, &physicalDeviceCount, physicalDevices.data());

    // For now, just pick the first device
    physicalDevice = physicalDevices[0];

    if (physicalDevice == VK_NULL_HANDLE) {
        throw std::runtime_error("Failed to find a suitable GPU");
    }
}

void RHI_Vulkan::createLogicalDevice() {
    // Find queue families (graphics and present)
    uint32_t queueFamilyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &queueFamilyCount, nullptr);
    std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &queueFamilyCount, queueFamilies.data());

    graphicsFamilyIdx = UINT32_MAX;
    presentFamilyIdx = UINT32_MAX;
    uint32_t i = 0;
    for (const auto& queueFamily : queueFamilies) {
        if (graphicsFamilyIdx == UINT32_MAX && queueFamily.queueCount > 0 && queueFamily.queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            graphicsFamilyIdx = i;
        }
        if (presentFamilyIdx == UINT32_MAX) {
            VkBool32 presentSupport;
            vkGetPhysicalDeviceSurfaceSupportKHR(physicalDevice, i, surface, &presentSupport);
            if (presentSupport) {
                presentFamilyIdx = i;
            }
        }
        ++i;
    }

    if (graphicsFamilyIdx == UINT32_MAX || presentFamilyIdx == UINT32_MAX) {
        throw std::runtime_error("Failed to find suitable queue families");
    }

    // Create device queue infos
    float queuePriority = 1.0f;
    VkDeviceQueueCreateInfo graphicsQueueInfo{};
    graphicsQueueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    graphicsQueueInfo.queueFamilyIndex = graphicsFamilyIdx;
    graphicsQueueInfo.queueCount = 1;
    graphicsQueueInfo.pQueuePriorities = &queuePriority;

    VkDeviceQueueCreateInfo presentQueueInfo{};
    presentQueueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    presentQueueInfo.queueFamilyIndex = presentFamilyIdx;
    presentQueueInfo.queueCount = 1;
    presentQueueInfo.pQueuePriorities = &queuePriority;

    // Device features
    VkPhysicalDeviceFeatures deviceFeatures{};
    deviceFeatures.samplerAnisotropy = VK_TRUE;

    VkPhysicalDeviceDynamicRenderingFeaturesKHR dynamicRenderingFeatures{};
    dynamicRenderingFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES_KHR;
    dynamicRenderingFeatures.dynamicRendering = VK_TRUE;

    VkPhysicalDeviceSynchronization2Features synchronization2Features{};
    synchronization2Features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES;
    synchronization2Features.pNext = &dynamicRenderingFeatures;
    synchronization2Features.synchronization2 = VK_TRUE;

    // Device extensions. Required ones fail device creation when missing;
    // optional ones (e.g. portability_subset, which only exists on MoltenVK)
    // are added only if the driver advertises them.
    std::vector<const char*> deviceExtensions = {
        VK_KHR_SWAPCHAIN_EXTENSION_NAME,
        VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME,
        VK_KHR_DEPTH_STENCIL_RESOLVE_EXTENSION_NAME,
        VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME,
        VK_KHR_MULTIVIEW_EXTENSION_NAME,
        VK_KHR_MAINTENANCE_2_EXTENSION_NAME,
        VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME
    };
    {
        uint32_t extCount = 0;
        vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &extCount, nullptr);
        std::vector<VkExtensionProperties> available(extCount);
        vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &extCount, available.data());
        auto has = [&](const char* name) {
            for (const auto& e : available)
                if (std::strcmp(e.extensionName, name) == 0) return true;
            return false;
        };
        // MoltenVK requires enabling portability_subset when it is present
        if (has(VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME)) {
            deviceExtensions.push_back(VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME);
        }
    }

    // Create device
    VkDeviceCreateInfo deviceInfo{};
    deviceInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    deviceInfo.pNext = &synchronization2Features;
    deviceInfo.queueCreateInfoCount = 1;
    deviceInfo.pQueueCreateInfos = &graphicsQueueInfo;
    deviceInfo.enabledExtensionCount = static_cast<uint32_t>(deviceExtensions.size());
    deviceInfo.ppEnabledExtensionNames = deviceExtensions.data();
    deviceInfo.pEnabledFeatures = &deviceFeatures;

    // If graphics and present are different families, create both queues
    if (graphicsFamilyIdx != presentFamilyIdx) {
        const VkDeviceQueueCreateInfo queueCreateInfos[2] = { graphicsQueueInfo, presentQueueInfo };
        deviceInfo.pQueueCreateInfos = queueCreateInfos;
        deviceInfo.queueCreateInfoCount = 2;
    }

    if (vkCreateDevice(physicalDevice, &deviceInfo, nullptr, &device) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create logical device");
    }

    // Get queue handles
    vkGetDeviceQueue(device, graphicsFamilyIdx, 0, &graphicsQueue);
    vkGetDeviceQueue(device, presentFamilyIdx, 0, &presentQueue);

    // Load extension function pointers
    vkCmdBeginRenderingKHR = (PFN_vkCmdBeginRenderingKHR)vkGetDeviceProcAddr(device, "vkCmdBeginRenderingKHR");
    vkCmdEndRenderingKHR = (PFN_vkCmdEndRenderingKHR)vkGetDeviceProcAddr(device, "vkCmdEndRenderingKHR");

    if (!vkCmdBeginRenderingKHR || !vkCmdEndRenderingKHR) {
        throw std::runtime_error("Failed to load dynamic rendering extension functions");
    }
}

// Tear down and rebuild the swapchain at the surface's current extent.
// createSwapchain() re-queries capabilities/formats itself, so this is a
// full-idle destroy + fresh create; renderers detect the extent change via
// getSwapchainWidth/Height and rebuild their own targets.
void RHI_Vulkan::recreateSwapchain() {
    if (device == VK_NULL_HANDLE) return;
    vkDeviceWaitIdle(device);

    for (auto& view : swapchainImageViews) {
        if (view != VK_NULL_HANDLE) vkDestroyImageView(device, view, nullptr);
    }
    swapchainImageViews.clear();
    swapchainImages.clear();
    if (swapchain != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(device, swapchain, nullptr);
        swapchain = VK_NULL_HANDLE;
    }

    createSwapchain();
    swapchainImageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
}

void RHI_Vulkan::createSwapchain() {
    // Get surface capabilities
    VkSurfaceCapabilitiesKHR capabilities;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physicalDevice, surface, &capabilities);

    // Determine extent
    VkExtent2D extent = capabilities.currentExtent;
    if (capabilities.currentExtent.width == UINT32_MAX) {
        int windowWidth, windowHeight;
        SDL_GetWindowSize(window, &windowWidth, &windowHeight);
        VkExtent2D actualExtent = { static_cast<uint32_t>(windowWidth), static_cast<uint32_t>(windowHeight) };
        actualExtent.width = std::max(
            capabilities.minImageExtent.width,
            std::min(capabilities.maxImageExtent.width, actualExtent.width));
        actualExtent.height = std::max(
            capabilities.minImageExtent.height,
            std::min(capabilities.maxImageExtent.height, actualExtent.height));
        extent = actualExtent;
    }
    swapchainExtent = extent;

    // Get surface formats
    std::vector<VkSurfaceFormatKHR> surfaceFormats;
    uint32_t surfaceFormatCount;
    vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, surface, &surfaceFormatCount, nullptr);
    if (surfaceFormatCount != 0) {
        surfaceFormats.resize(surfaceFormatCount);
        vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, surface, &surfaceFormatCount, surfaceFormats.data());
    }

    // Select surface format
    VkSurfaceFormatKHR selectedSurfaceFormat = surfaceFormats[0];
    for (const auto& surfaceFormat : surfaceFormats) {
        if (surfaceFormat.format == VK_FORMAT_B8G8R8A8_SRGB && surfaceFormat.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            selectedSurfaceFormat = surfaceFormat;
            break;
        }
    }
    swapchainImageFormat = selectedSurfaceFormat.format;

    // Get present modes
    std::vector<VkPresentModeKHR> presentModes;
    uint32_t presentModeCount;
    vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevice, surface, &presentModeCount, nullptr);
    if (presentModeCount != 0) {
        presentModes.resize(presentModeCount);
        vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevice, surface, &presentModeCount, presentModes.data());
    }

    // Select present mode (prefer mailbox)
    VkPresentModeKHR selectedPresentMode = VK_PRESENT_MODE_FIFO_KHR;
    for (const auto& presentMode : presentModes) {
        if (presentMode == VK_PRESENT_MODE_MAILBOX_KHR) {
            selectedPresentMode = presentMode;
            break;
        }
    }

    // Determine image count
    uint32_t swapchainImageCount = std::max(capabilities.minImageCount, MAX_FRAMES_IN_FLIGHT);
    if (capabilities.maxImageCount > 0 && swapchainImageCount > capabilities.maxImageCount) {
        swapchainImageCount = capabilities.maxImageCount;
    }

    // Create swapchain
    const uint32_t queueFamilyIndices[2] = { graphicsFamilyIdx, presentFamilyIdx };
    VkSwapchainCreateInfoKHR swapchainInfo{};
    swapchainInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    swapchainInfo.surface = surface;
    swapchainInfo.minImageCount = swapchainImageCount;
    swapchainInfo.imageFormat = selectedSurfaceFormat.format;
    swapchainInfo.imageColorSpace = selectedSurfaceFormat.colorSpace;
    swapchainInfo.imageExtent = extent;
    swapchainInfo.imageArrayLayers = 1;
    swapchainInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    swapchainInfo.preTransform = capabilities.currentTransform;
    swapchainInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    swapchainInfo.presentMode = selectedPresentMode;
    swapchainInfo.clipped = VK_TRUE;
    swapchainInfo.oldSwapchain = VK_NULL_HANDLE;

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
        throw std::runtime_error("Failed to create swapchain");
    }

    // Get swapchain images
    vkGetSwapchainImagesKHR(device, swapchain, &swapchainImageCount, nullptr);
    swapchainImages.resize(swapchainImageCount);
    vkGetSwapchainImagesKHR(device, swapchain, &swapchainImageCount, swapchainImages.data());

    // Create image views for swapchain images
    swapchainImageViews.resize(swapchainImageCount);
    for (size_t i = 0; i < swapchainImageCount; i++) {
        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = swapchainImages[i];
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = swapchainImageFormat;
        viewInfo.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
        viewInfo.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
        viewInfo.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
        viewInfo.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.baseMipLevel = 0;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.baseArrayLayer = 0;
        viewInfo.subresourceRange.layerCount = 1;

        if (vkCreateImageView(device, &viewInfo, nullptr, &swapchainImageViews[i]) != VK_SUCCESS) {
            throw std::runtime_error("Failed to create swapchain image views");
        }
    }
}

void RHI_Vulkan::createCommandPool() {
    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = graphicsFamilyIdx;

    if (vkCreateCommandPool(device, &poolInfo, nullptr, &commandPool) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create command pool");
    }
}

void RHI_Vulkan::createCommandBuffers() {
    commandBuffers.resize(MAX_FRAMES_IN_FLIGHT);

    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = commandPool;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = static_cast<uint32_t>(commandBuffers.size());

    if (vkAllocateCommandBuffers(device, &allocInfo, commandBuffers.data()) != VK_SUCCESS) {
        throw std::runtime_error("Failed to allocate command buffers");
    }
}

void RHI_Vulkan::createSyncObjects() {
    imageAvailableSemaphores.resize(MAX_FRAMES_IN_FLIGHT);
    // Present-wait semaphores must be per swapchain image, not per frame in
    // flight: the semaphore passed to vkQueuePresentKHR for an image may still
    // be in use when a frame-indexed slot comes around again.
    renderFinishedSemaphores.resize(swapchainImages.size());
    inFlightFences.resize(MAX_FRAMES_IN_FLIGHT);

    VkSemaphoreCreateInfo semaphoreInfo{};
    semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

    for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        if (vkCreateSemaphore(device, &semaphoreInfo, nullptr, &imageAvailableSemaphores[i]) != VK_SUCCESS ||
            vkCreateFence(device, &fenceInfo, nullptr, &inFlightFences[i]) != VK_SUCCESS) {
            throw std::runtime_error("Failed to create sync objects");
        }
    }
    for (size_t i = 0; i < renderFinishedSemaphores.size(); i++) {
        if (vkCreateSemaphore(device, &semaphoreInfo, nullptr, &renderFinishedSemaphores[i]) != VK_SUCCESS) {
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
        case PixelFormat::RG32_FLOAT: return VK_FORMAT_R32G32_SFLOAT;
        case PixelFormat::RGB32_FLOAT: return VK_FORMAT_R32G32B32_SFLOAT;
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
    // TRANSFER_DST everywhere: DEVICE_LOCAL buffers are filled via staging
    // copies. Uniform also carries STORAGE because the RHI binding model
    // exposes every buffer binding as an SSBO (std430) — see the descriptor
    // binding model notes in rhi_vulkan.hpp.
    switch (usage) {
        case BufferUsage::Vertex:
            return VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        case BufferUsage::Index:
            return VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        case BufferUsage::Uniform:
            return VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                   VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        case BufferUsage::Storage:
            return VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        case BufferUsage::TransferSrc: return VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        case BufferUsage::TransferDst: return VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        default: return VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    }
}

VkImageUsageFlags RHI_Vulkan::convertTextureUsage(TextureUsage usage) {
    VkImageUsageFlags flags = 0;
    if (hasUsage(usage, TextureUsage::Sampled)) {
        // TRANSFER_SRC allows generateMipmaps() to blit between levels
        flags |= VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                 VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    }
    if (hasUsage(usage, TextureUsage::Storage)) {
        flags |= VK_IMAGE_USAGE_STORAGE_BIT;
    }
    if (hasUsage(usage, TextureUsage::RenderTarget)) {
        flags |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    }
    if (hasUsage(usage, TextureUsage::DepthStencil)) {
        flags |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    }
    return flags != 0 ? flags : VK_IMAGE_USAGE_SAMPLED_BIT;
}

// ============================================================================
// Compute Pipeline
// ============================================================================

ComputePipelineHandle RHI_Vulkan::createComputePipeline(const ComputePipelineDesc& desc) {
    auto it = shaders.find(desc.computeShader.id);
    if (it == shaders.end()) {
        throw std::runtime_error("Invalid compute shader handle");
    }

    VkComputePipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipelineInfo.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pipelineInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pipelineInfo.stage.module = it->second.module;
    pipelineInfo.stage.pName = "main";
    // All compute pipelines share the global compute layout
    pipelineInfo.layout = computePipelineLayout;

    VkPipeline pipeline;
    if (vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create compute pipeline");
    }

    Uint32 id = nextComputePipelineId++;
    computePipelines[id] = {pipeline, VK_NULL_HANDLE};

    return ComputePipelineHandle{id};
}

void RHI_Vulkan::destroyComputePipeline(ComputePipelineHandle handle) {
    auto it = computePipelines.find(handle.id);
    if (it != computePipelines.end()) {
        if (it->second.pipeline != VK_NULL_HANDLE) {
            vkDestroyPipeline(device, it->second.pipeline, nullptr);
        }
        // layout is the shared compute pipeline layout — not owned per pipeline
        computePipelines.erase(it);
    }
}

// ============================================================================
// Acceleration Structures (Stub - requires VK_KHR_acceleration_structure)
// ============================================================================

AccelStructHandle RHI_Vulkan::createAccelerationStructure(const AccelStructDesc& desc) {
    // Vulkan ray tracing not implemented yet
    // This is a stub for API compatibility
    fmt::print("Warning: Vulkan acceleration structures not yet implemented\n");

    Uint32 id = nextAccelStructId++;
    accelStructs[id] = {VK_NULL_HANDLE, VK_NULL_HANDLE, nullptr};

    return AccelStructHandle{id};
}

void RHI_Vulkan::destroyAccelerationStructure(AccelStructHandle handle) {
    accelStructs.erase(handle.id);
}

void RHI_Vulkan::buildAccelerationStructure(AccelStructHandle handle) {
    // Stub
}

void RHI_Vulkan::updateAccelerationStructure(AccelStructHandle handle, const std::vector<AccelStructInstance>& instances) {
    // Stub
}

// ============================================================================
// Compute Commands
// ============================================================================

void RHI_Vulkan::beginComputePass() {
    // Compute shares the frame command buffer; just instrument the region
    if (currentCommandBuffer == VK_NULL_HANDLE) {
        return;
    }
    beginDebugLabel(currentCommandBuffer, "Compute");
    Uint32 tsBegin, tsEnd;
    if (allocateTimestampPair("Compute", tsBegin, tsEnd)) {
        vkCmdWriteTimestamp(currentCommandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                            timestampPools[currentFrameInFlight], tsBegin);
        currentPassEndQuery = tsEnd;
    } else {
        currentPassEndQuery = UINT32_MAX;
    }
}

void RHI_Vulkan::endComputePass() {
    if (currentCommandBuffer == VK_NULL_HANDLE) {
        return;
    }
    if (currentPassEndQuery != UINT32_MAX) {
        vkCmdWriteTimestamp(currentCommandBuffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                            timestampPools[currentFrameInFlight], currentPassEndQuery);
        currentPassEndQuery = UINT32_MAX;
    }
    endDebugLabel(currentCommandBuffer);
}

void RHI_Vulkan::bindComputePipeline(ComputePipelineHandle pipeline) {
    auto it = computePipelines.find(pipeline.id);
    if (it != computePipelines.end() && currentCommandBuffer) {
        vkCmdBindPipeline(currentCommandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, it->second.pipeline);
    }
}

void RHI_Vulkan::setComputeBuffer(Uint32 binding, BufferHandle buffer, size_t offset, size_t range) {
    auto it = buffers.find(buffer.id);
    if (it == buffers.end() || binding >= BINDINGS_PER_SET) {
        return;
    }
    boundComputeBuffers[binding] = { it->second.buffer, offset, range > 0 ? range : VK_WHOLE_SIZE };
    computeDescriptorsDirty = true;
}

void RHI_Vulkan::setComputeBytes(const void* data, size_t size, Uint32 binding) {
    // Compute push constants, mirroring the graphics-bytes convention:
    // 16-byte slot at (binding % 4) * 16 within the compute push range.
    if (currentCommandBuffer == VK_NULL_HANDLE || !data || size == 0) return;
    Uint32 offset = (binding % 4) * 16;
    Uint32 clamped = static_cast<Uint32>(std::min<size_t>(size, 64 - offset));
    vkCmdPushConstants(currentCommandBuffer, computePipelineLayout,
                       VK_SHADER_STAGE_COMPUTE_BIT, offset, clamped, data);
}

void RHI_Vulkan::setComputeTexture(Uint32 binding, TextureHandle texture) {
    auto it = textures.find(texture.id);
    if (it == textures.end() || binding >= BINDINGS_PER_SET) {
        return;
    }
    // Storage images are read/written in GENERAL layout. This runs outside
    // any render pass (compute passes), so a barrier here is legal.
    transitionImage(it->second.image, it->second.currentLayout,
                    VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_ASPECT_COLOR_BIT);
    it->second.currentLayout = VK_IMAGE_LAYOUT_GENERAL;
    boundComputeImages[binding] = it->second.view;
    computeDescriptorsDirty = true;
}

void RHI_Vulkan::flushComputeDescriptors() {
    if (!computeDescriptorsDirty || currentCommandBuffer == VK_NULL_HANDLE) {
        return;
    }
    computeDescriptorsDirty = false;

    VkDescriptorSetLayout layouts[2] = { computeBufferSetLayout, computeImageSetLayout };
    VkDescriptorSet sets[2];
    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = descriptorPools[currentFrameInFlight];
    allocInfo.descriptorSetCount = 2;
    allocInfo.pSetLayouts = layouts;
    if (vkAllocateDescriptorSets(device, &allocInfo, sets) != VK_SUCCESS) {
        fmt::print(stderr, "flushComputeDescriptors: descriptor pool exhausted\n");
        return;
    }

    VkWriteDescriptorSet writes[BINDINGS_PER_SET * 2];
    VkDescriptorBufferInfo bufferInfos[BINDINGS_PER_SET];
    VkDescriptorImageInfo imageInfos[BINDINGS_PER_SET];
    Uint32 writeCount = 0, bufferCount = 0, imageCount = 0;

    for (Uint32 i = 0; i < BINDINGS_PER_SET; i++) {
        if (boundComputeBuffers[i].buffer == VK_NULL_HANDLE) continue;
        VkDescriptorBufferInfo& info = bufferInfos[bufferCount++];
        info = { boundComputeBuffers[i].buffer, boundComputeBuffers[i].offset, boundComputeBuffers[i].range };
        VkWriteDescriptorSet& w = writes[writeCount++];
        w = {};
        w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w.dstSet = sets[0];
        w.dstBinding = i;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w.pBufferInfo = &info;
    }
    for (Uint32 i = 0; i < BINDINGS_PER_SET; i++) {
        if (boundComputeImages[i] == VK_NULL_HANDLE) continue;
        VkDescriptorImageInfo& info = imageInfos[imageCount++];
        info = { VK_NULL_HANDLE, boundComputeImages[i], VK_IMAGE_LAYOUT_GENERAL };
        VkWriteDescriptorSet& w = writes[writeCount++];
        w = {};
        w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w.dstSet = sets[1];
        w.dstBinding = i;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        w.pImageInfo = &info;
    }

    if (writeCount > 0) {
        vkUpdateDescriptorSets(device, writeCount, writes, 0, nullptr);
    }
    vkCmdBindDescriptorSets(currentCommandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                            computePipelineLayout, 0, 2, sets, 0, nullptr);
}

void RHI_Vulkan::setAccelerationStructure(Uint32 binding, AccelStructHandle accelStruct) {
    // TODO: Implement acceleration structure binding
    // For now, this is a stub
}

void RHI_Vulkan::dispatch(Uint32 groupCountX, Uint32 groupCountY, Uint32 groupCountZ) {
    if (currentCommandBuffer) {
        flushComputeDescriptors();
        vkCmdDispatch(currentCommandBuffer, groupCountX, groupCountY, groupCountZ);
    }
}

void RHI_Vulkan::setScissor(int32_t x, int32_t y, Uint32 width, Uint32 height) {
    if (currentCommandBuffer == VK_NULL_HANDLE) return;
    // Clamp to non-negative offsets (Vulkan requires offset >= 0).
    if (x < 0) { width += x; x = 0; }
    if (y < 0) { height += y; y = 0; }
    VkRect2D scissor{};
    scissor.offset = { x, y };
    scissor.extent = { width, height };
    vkCmdSetScissor(currentCommandBuffer, 0, 1, &scissor);
}

void RHI_Vulkan::computeBarrier() {
    if (currentCommandBuffer == VK_NULL_HANDLE) return;
    VkMemoryBarrier b{};
    b.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    b.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(currentCommandBuffer,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0, 1, &b, 0, nullptr, 0, nullptr);
}

// ============================================================================
// Factory Function
// ============================================================================

RHI* createRHIVulkan() {
    return new RHI_Vulkan();
}
