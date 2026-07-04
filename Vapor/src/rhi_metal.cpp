#define NS_PRIVATE_IMPLEMENTATION
#define MTL_PRIVATE_IMPLEMENTATION
#define CA_PRIVATE_IMPLEMENTATION

#include "rhi_metal.hpp"
#include <fmt/core.h>
#include <stdexcept>
#include <cstdlib>
#include <cstring>

// ============================================================================
// Factory Function
// ============================================================================

RHI* createRHIMetal() {
    return new RHI_Metal();
}

// ============================================================================
// Constructor / Destructor
// ============================================================================

RHI_Metal::RHI_Metal() {
}

RHI_Metal::~RHI_Metal() {
    shutdown();
}

// ============================================================================
// Initialization
// ============================================================================

bool RHI_Metal::initialize(SDL_Window* window) {
    this->window = window;

    // Create SDL renderer (needed for Metal layer access)
    renderer = SDL_CreateRenderer(window, nullptr);
    if (!renderer) {
        fmt::print("Failed to create SDL renderer\n");
        return false;
    }

    // Get Metal layer from SDL
    swapchain = (CA::MetalLayer*)SDL_GetRenderMetalLayer(renderer);
    if (!swapchain) {
        fmt::print("Failed to get Metal layer\n");
        return false;
    }

    // Configure swapchain
    swapchain->setPixelFormat(MTL::PixelFormatRGBA8Unorm_sRGB);
    swapchain->setColorspace(CGColorSpaceCreateWithName(kCGColorSpaceSRGB));
    // Allow reading back the drawable (e.g. screenshot blit). CAMetalLayer
    // drawables default to framebufferOnly=YES, which forbids using them as a
    // blit/copy source and triggers a Metal validation assertion (crash) in
    // copySwapchainToBuffer().
    swapchain->setFramebufferOnly(false);

    // Get device from swapchain
    device = swapchain->device();
    if (!device) {
        fmt::print("Failed to get Metal device\n");
        return false;
    }

    // Create command queue
    commandQueue = NS::TransferPtr(device->newCommandQueue());
    if (!commandQueue) {
        fmt::print("Failed to create command queue\n");
        return false;
    }

    // Get swapchain dimensions in PIXELS (not logical points).
    // On HiDPI/Retina displays the backing scale is >1, so the window's pixel
    // size is larger than its logical size. The Metal drawable works in pixels;
    // using logical points here would size the drawable to a fraction of the
    // layer (e.g. 1/4 on a 2x display), rendering only the top-left corner and
    // leaving the rest showing stale/garbage content.
    int width, height;
    SDL_GetWindowSizeInPixels(window, &width, &height);
    swapchainWidth = static_cast<Uint32>(width);
    swapchainHeight = static_cast<Uint32>(height);

    // Pin the layer's drawable size to the pixel dimensions so the drawable,
    // viewport and render targets all agree.
    swapchain->setDrawableSize(CGSize{ static_cast<CGFloat>(width), static_cast<CGFloat>(height) });

    initGpuTiming();

    // Device capabilities. Raytracing is force-disabled on CI runners: the
    // virtualized GPU advertises support but acceleration-structure builds
    // fail there (matching the old renderer's behavior).
    capabilities.raytracing = device->supportsRaytracing() && !std::getenv("GITHUB_ACTIONS");
    capabilities.computeShaders = true;
    capabilities.gpuTimestamps = gpuTimingSupported;

    return true;
}

// ============================================================================
// GPU Pass Timing
// ============================================================================

void RHI_Metal::initGpuTiming() {
    // Timestamp sampling at stage boundaries (Apple Silicon). Each pass embeds
    // begin/end samples via descriptor-level sampleBufferAttachments.
    if (!device->supportsCounterSampling(MTL::CounterSamplingPointAtStageBoundary)) {
        return;
    }

    auto counterSets = device->counterSets();
    for (NS::UInteger i = 0; counterSets && i < counterSets->count(); ++i) {
        auto cs = static_cast<MTL::CounterSet*>(counterSets->object(i));
        if (cs->name()->isEqualToString(MTL::CommonCounterSetTimestamp)) {
            auto desc = NS::TransferPtr(MTL::CounterSampleBufferDescriptor::alloc()->init());
            desc->setCounterSet(cs);
            desc->setSampleCount(GPU_TIMER_SAMPLE_COUNT);
            desc->setStorageMode(MTL::StorageModeShared);
            NS::Error* err = nullptr;
            gpuTimerSampleBuffer = NS::TransferPtr(device->newCounterSampleBuffer(desc.get(), &err));
            if (gpuTimerSampleBuffer && !err) {
                gpuTimingSupported = true;
            }
            break;
        }
    }
}

bool RHI_Metal::allocateTimingSlots(const char* passName, NS::UInteger& outBegin, NS::UInteger& outEnd) {
    if (!gpuTimingActiveThisFrame || !gpuTimerSampleBuffer) {
        return false;
    }
    if (nextTimingSlot + 2 > GPU_TIMER_SAMPLE_COUNT) {
        return false;  // per-frame slot budget exhausted; pass simply goes untimed
    }
    outBegin = nextTimingSlot;
    outEnd = nextTimingSlot + 1;
    nextTimingSlot += 2;
    framePassSamples.push_back({passName ? passName : "(unnamed pass)", outBegin, outEnd});
    return true;
}

void RHI_Metal::resolveGpuTimings() {
    if (framePassSamples.empty() || !gpuTimerSampleBuffer || !currentCommandBuffer) {
        return;
    }

    // Capture by value: the handler runs after endFrame() has reset frame state.
    auto capturedInfo = framePassSamples;
    auto capturedBuf = gpuTimerSampleBuffer;  // retain via SharedPtr copy
    NS::UInteger sampleCount = capturedInfo.back().endIdx + 1;
    currentCommandBuffer->addCompletedHandler([this, capturedInfo, capturedBuf, sampleCount](MTL::CommandBuffer*) {
        NS::Data* data = capturedBuf->resolveCounterRange(NS::Range::Make(0, sampleCount));
        if (!data) return;
        auto* timestamps = reinterpret_cast<const MTL::CounterResultTimestamp*>(data->mutableBytes());
        std::lock_guard<std::mutex> lock(gpuTimingMutex);
        gpuPassTimings.clear();
        gpuPassTimings.reserve(capturedInfo.size());
        for (const auto& info : capturedInfo) {
            uint64_t begin = timestamps[info.beginIdx].timestamp;
            uint64_t end = timestamps[info.endIdx].timestamp;
            double ms = (end >= begin) ? static_cast<double>(end - begin) / 1e6 : 0.0;
            gpuPassTimings.push_back({info.name, ms});
        }
    });
}

std::vector<GpuPassTiming> RHI_Metal::getGpuPassTimings() {
    std::lock_guard<std::mutex> lock(gpuTimingMutex);
    return gpuPassTimings;
}

void RHI_Metal::shutdown() {
    if (renderer) {
        // Wait for GPU to finish
        waitIdle();

        // Clear all resources
        buffers.clear();
        textures.clear();
        shaders.clear();
        samplers.clear();
        pipelines.clear();
        computePipelines.clear();
        accelStructs.clear();
        gpuTimerSampleBuffer = nullptr;

        // Release command queue
        commandQueue = nullptr;

        // Destroy SDL renderer
        SDL_DestroyRenderer(renderer);
        renderer = nullptr;
    }

    device = nullptr;
    swapchain = nullptr;
    window = nullptr;
}

void RHI_Metal::waitIdle() {
    if (commandQueue) {
        // Submit empty command buffer and wait
        auto commandBuffer = commandQueue->commandBuffer();
        if (commandBuffer) {
            commandBuffer->commit();
            commandBuffer->waitUntilCompleted();
        }
    }
}

// ============================================================================
// Resource Creation - Buffer
// ============================================================================

BufferHandle RHI_Metal::createBuffer(const BufferDesc& desc) {
    MTL::ResourceOptions options = MTL::ResourceStorageModeManaged;

    switch (desc.memoryUsage) {
        case MemoryUsage::GPU:
            options = MTL::ResourceStorageModePrivate;
            break;
        case MemoryUsage::CPU:
            options = MTL::ResourceStorageModeShared;
            break;
        case MemoryUsage::CPUtoGPU:
            options = MTL::ResourceStorageModeManaged;
            break;
        case MemoryUsage::GPUreadback:
            options = MTL::ResourceStorageModeShared;
            break;
    }

    auto buffer = NS::TransferPtr(device->newBuffer(desc.size, options));
    if (!buffer) {
        throw std::runtime_error("Failed to create Metal buffer");
    }

    Uint32 id = nextBufferId++;
    buffers[id] = {buffer, desc.size, false, nullptr};

    return BufferHandle{id};
}

void RHI_Metal::destroyBuffer(BufferHandle handle) {
    buffers.erase(handle.id);
}

// ============================================================================
// Resource Creation - Texture
// ============================================================================

TextureHandle RHI_Metal::createTexture(const TextureDesc& desc) {
    auto textureDesc = MTL::TextureDescriptor::alloc()->init();

    // Set texture type based on sample count (MSAA uses multisample type)
    if (desc.sampleCount > 1) {
        textureDesc->setTextureType(MTL::TextureType2DMultisample);
    } else {
        textureDesc->setTextureType(MTL::TextureType2D);
    }

    textureDesc->setWidth(desc.width);
    textureDesc->setHeight(desc.height);
    textureDesc->setDepth(desc.depth);
    textureDesc->setMipmapLevelCount(desc.mipLevels);
    textureDesc->setArrayLength(desc.arrayLayers);
    textureDesc->setSampleCount(desc.sampleCount);
    textureDesc->setPixelFormat(convertPixelFormat(desc.format));
    textureDesc->setUsage(convertTextureUsage(desc.usage));
    textureDesc->setStorageMode(MTL::StorageModePrivate);

    auto texture = NS::TransferPtr(device->newTexture(textureDesc));
    textureDesc->release();

    if (!texture) {
        throw std::runtime_error("Failed to create Metal texture");
    }

    Uint32 id = nextTextureId++;
    textures[id] = {
        texture,
        desc.width,
        desc.height,
        desc.depth,
        desc.mipLevels,
        convertPixelFormat(desc.format)
    };

    return TextureHandle{id};
}

void RHI_Metal::destroyTexture(TextureHandle handle) {
    textures.erase(handle.id);
}

// ============================================================================
// Resource Creation - Shader
// ============================================================================

ShaderHandle RHI_Metal::createShader(const ShaderDesc& desc) {
    NS::SharedPtr<MTL::Library> library;
    NS::SharedPtr<MTL::Function> function;

    if (desc.code && desc.codeSize > 0) {
        // Create library from source code
        std::string source(static_cast<const char*>(desc.code), desc.codeSize);
        NS::Error* error = nullptr;
        auto sourceString = NS::String::string(source.c_str(), NS::UTF8StringEncoding);
        library = NS::TransferPtr(device->newLibrary(sourceString, nullptr, &error));

        if (!library || error) {
            if (error) {
                auto errorDesc = error->localizedDescription()->utf8String();
                fmt::print("Shader compilation error: {}\n", errorDesc);
            }
            throw std::runtime_error("Failed to create shader library");
        }

        // Get function by entry point name (use default Metal naming if not specified)
        std::string entryPoint;
        if (desc.entryPoint && strlen(desc.entryPoint) > 0) {
            entryPoint = desc.entryPoint;
        } else {
            // Use default Metal naming convention based on shader stage
            switch (desc.stage) {
                case ShaderStage::Vertex:
                    entryPoint = "vertexMain";
                    break;
                case ShaderStage::Fragment:
                    entryPoint = "fragmentMain";
                    break;
                case ShaderStage::Compute:
                    entryPoint = "computeMain";
                    break;
                default:
                    entryPoint = "main";
                    break;
            }
        }
        function = NS::TransferPtr(library->newFunction(NS::String::string(entryPoint.c_str(), NS::UTF8StringEncoding)));
        if (!function) {
            throw std::runtime_error(fmt::format("Failed to find shader entry point '{}'", entryPoint));
        }
    }
    // Note: ShaderDesc only supports code/codeSize, not filepath
    // If file loading is needed, it should be done before creating ShaderDesc

    Uint32 id = nextShaderId++;
    shaders[id] = {library, function, desc.stage};

    return ShaderHandle{id};
}

void RHI_Metal::destroyShader(ShaderHandle handle) {
    shaders.erase(handle.id);
}

// ============================================================================
// Resource Creation - Sampler
// ============================================================================

SamplerHandle RHI_Metal::createSampler(const SamplerDesc& desc) {
    auto samplerDesc = MTL::SamplerDescriptor::alloc()->init();
    samplerDesc->setMinFilter(convertSamplerFilter(desc.minFilter));
    samplerDesc->setMagFilter(convertSamplerFilter(desc.magFilter));
    samplerDesc->setMipFilter(convertSamplerMipFilter(desc.mipFilter));
    samplerDesc->setSAddressMode(convertSamplerAddressMode(desc.addressModeU));
    samplerDesc->setTAddressMode(convertSamplerAddressMode(desc.addressModeV));
    samplerDesc->setRAddressMode(convertSamplerAddressMode(desc.addressModeW));
    samplerDesc->setMaxAnisotropy(desc.enableAnisotropy ? desc.maxAnisotropy : 1);
    samplerDesc->setCompareFunction(convertCompareOp(desc.compareOp));

    auto sampler = NS::TransferPtr(device->newSamplerState(samplerDesc));
    samplerDesc->release();

    if (!sampler) {
        throw std::runtime_error("Failed to create sampler");
    }

    Uint32 id = nextSamplerId++;
    samplers[id] = {sampler};

    return SamplerHandle{id};
}

void RHI_Metal::destroySampler(SamplerHandle handle) {
    samplers.erase(handle.id);
}

// ============================================================================
// Resource Creation - Pipeline
// ============================================================================

PipelineHandle RHI_Metal::createPipeline(const PipelineDesc& desc) {
    auto pipelineDesc = MTL::RenderPipelineDescriptor::alloc()->init();

    // Get shaders
    auto vsIt = shaders.find(desc.vertexShader.id);
    auto fsIt = shaders.find(desc.fragmentShader.id);
    if (vsIt == shaders.end() || fsIt == shaders.end()) {
        pipelineDesc->release();
        throw std::runtime_error("Invalid shader handles for pipeline");
    }

    pipelineDesc->setVertexFunction(vsIt->second.function.get());
    pipelineDesc->setFragmentFunction(fsIt->second.function.get());

    // Color attachment
    auto colorAttachment = pipelineDesc->colorAttachments()->object(0);
    colorAttachment->setPixelFormat(swapchainFormat);

    // Blending
    switch (desc.blendMode) {
        case BlendMode::Opaque:
            colorAttachment->setBlendingEnabled(false);
            break;
        case BlendMode::AlphaBlend:
            colorAttachment->setBlendingEnabled(true);
            colorAttachment->setSourceRGBBlendFactor(MTL::BlendFactorSourceAlpha);
            colorAttachment->setDestinationRGBBlendFactor(MTL::BlendFactorOneMinusSourceAlpha);
            colorAttachment->setRgbBlendOperation(MTL::BlendOperationAdd);
            colorAttachment->setSourceAlphaBlendFactor(MTL::BlendFactorOne);
            colorAttachment->setDestinationAlphaBlendFactor(MTL::BlendFactorZero);
            break;
        case BlendMode::Additive:
            colorAttachment->setBlendingEnabled(true);
            colorAttachment->setSourceRGBBlendFactor(MTL::BlendFactorOne);
            colorAttachment->setDestinationRGBBlendFactor(MTL::BlendFactorOne);
            colorAttachment->setRgbBlendOperation(MTL::BlendOperationAdd);
            colorAttachment->setSourceAlphaBlendFactor(MTL::BlendFactorOne);
            colorAttachment->setDestinationAlphaBlendFactor(MTL::BlendFactorOne);
            break;
        case BlendMode::Multiply:
            colorAttachment->setBlendingEnabled(true);
            colorAttachment->setSourceRGBBlendFactor(MTL::BlendFactorDestinationColor);
            colorAttachment->setDestinationRGBBlendFactor(MTL::BlendFactorZero);
            colorAttachment->setRgbBlendOperation(MTL::BlendOperationAdd);
            colorAttachment->setSourceAlphaBlendFactor(MTL::BlendFactorOne);
            colorAttachment->setDestinationAlphaBlendFactor(MTL::BlendFactorZero);
            colorAttachment->setAlphaBlendOperation(MTL::BlendOperationAdd);
            break;
    }

    // Depth attachment format must match the render pass this pipeline is
    // used in, regardless of whether depth testing is enabled.
    if (desc.hasDepthAttachment) {
        pipelineDesc->setDepthAttachmentPixelFormat(MTL::PixelFormatDepth32Float);
    }

    // Sample count
    pipelineDesc->setSampleCount(desc.sampleCount);

    // Create pipeline
    NS::Error* error = nullptr;
    auto pipeline = NS::TransferPtr(device->newRenderPipelineState(pipelineDesc, &error));
    pipelineDesc->release();

    if (!pipeline || error) {
        if (error) {
            auto errorDesc = error->localizedDescription()->utf8String();
            fmt::print("Pipeline creation error: {}\n", errorDesc);
        }
        throw std::runtime_error("Failed to create render pipeline");
    }

    // Depth-stencil state (Metal keeps this separate from the PSO; we bake it
    // into our pipeline object so bindPipeline() can apply everything at once).
    // Depth writes only apply while depth testing is enabled, matching Vulkan.
    NS::SharedPtr<MTL::DepthStencilState> depthStencilState;
    if (desc.hasDepthAttachment) {
        auto dsDesc = NS::TransferPtr(MTL::DepthStencilDescriptor::alloc()->init());
        dsDesc->setDepthCompareFunction(
            desc.depthTest ? convertCompareOp(desc.depthCompareOp) : MTL::CompareFunctionAlways);
        dsDesc->setDepthWriteEnabled(desc.depthTest && desc.depthWrite);
        depthStencilState = NS::TransferPtr(device->newDepthStencilState(dsDesc.get()));
    }

    Uint32 id = nextPipelineId++;
    PipelineResource resource;
    resource.renderPipeline = pipeline;
    resource.isCompute = false;
    resource.depthStencilState = depthStencilState;
    resource.cullMode = convertCullMode(desc.cullMode);
    resource.winding = convertFrontFace(desc.frontFaceCounterClockwise);
    resource.primitiveType = convertPrimitiveTopology(desc.topology);
    pipelines[id] = resource;

    return PipelineHandle{id};
}

void RHI_Metal::destroyPipeline(PipelineHandle handle) {
    pipelines.erase(handle.id);
}

// ============================================================================
// Resource Updates
// ============================================================================

void RHI_Metal::updateBuffer(BufferHandle handle, const void* data, size_t offset, size_t size) {
    auto it = buffers.find(handle.id);
    if (it == buffers.end()) {
        return;
    }

    MTL::Buffer* buffer = it->second.buffer.get();
    MTL::StorageMode storageMode = buffer->storageMode();

    if (storageMode == MTL::StorageModePrivate) {
        // GPU-only buffer: use staging buffer and blit encoder
        auto stagingBuffer = NS::TransferPtr(device->newBuffer(size, MTL::ResourceStorageModeShared));
        if (!stagingBuffer) {
            throw std::runtime_error("Failed to create staging buffer for updateBuffer");
        }

        std::memcpy(stagingBuffer->contents(), data, size);

        // Create a temporary command buffer for the copy
        auto cmdBuffer = NS::TransferPtr(commandQueue->commandBuffer());
        if (!cmdBuffer) {
            throw std::runtime_error("Failed to create command buffer for updateBuffer");
        }

        auto blitEncoder = cmdBuffer->blitCommandEncoder();
        if (!blitEncoder) {
            throw std::runtime_error("Failed to create blit encoder for updateBuffer");
        }

        blitEncoder->copyFromBuffer(stagingBuffer.get(), 0, buffer, offset, size);
        blitEncoder->endEncoding();
        cmdBuffer->commit();
        cmdBuffer->waitUntilCompleted();
    } else {
        // CPU-accessible buffer: direct write
        void* bufferData = buffer->contents();
        if (!bufferData) {
            throw std::runtime_error("Buffer contents() returned nullptr");
        }
        std::memcpy(static_cast<char*>(bufferData) + offset, data, size);

        // Notify Metal of modified range if managed storage
        if (storageMode == MTL::StorageModeManaged) {
            buffer->didModifyRange(NS::Range::Make(offset, size));
        }
    }
}

void RHI_Metal::updateTexture(TextureHandle handle, const void* data, size_t size) {
    auto it = textures.find(handle.id);
    if (it == textures.end()) {
        return;
    }

    const TextureResource& texRes = it->second;
    MTL::Texture* texture = texRes.texture.get();

    // Check storage mode - Private textures need staging buffer
    if (texture->storageMode() == MTL::StorageModePrivate) {
        // GPU-only texture: use staging buffer and blit encoder
        Uint32 bytesPerPixel = 4; // Assume RGBA8 for now
        Uint32 bytesPerRow = texRes.width * bytesPerPixel;
        Uint32 bytesPerImage = bytesPerRow * texRes.height;

        // Create staging buffer
        auto stagingBuffer = NS::TransferPtr(device->newBuffer(size, MTL::ResourceStorageModeShared));
        if (!stagingBuffer) {
            throw std::runtime_error("Failed to create staging buffer for updateTexture");
        }

        std::memcpy(stagingBuffer->contents(), data, size);

        // Create a temporary command buffer for the copy
        auto cmdBuffer = NS::TransferPtr(commandQueue->commandBuffer());
        if (!cmdBuffer) {
            throw std::runtime_error("Failed to create command buffer for updateTexture");
        }

        auto blitEncoder = cmdBuffer->blitCommandEncoder();
        if (!blitEncoder) {
            throw std::runtime_error("Failed to create blit encoder for updateTexture");
        }

        // Copy from staging buffer to texture
        // API: copyFromBuffer(sourceBuffer, sourceOffset, sourceBytesPerRow, sourceBytesPerImage, sourceSize,
        //                     destinationTexture, destinationSlice, destinationLevel, destinationOrigin)
        blitEncoder->copyFromBuffer(
            stagingBuffer.get(),                    // sourceBuffer
            0,                                      // sourceOffset
            bytesPerRow,                           // sourceBytesPerRow
            bytesPerImage,                         // sourceBytesPerImage
            MTL::Size::Make(texRes.width, texRes.height, 1),  // sourceSize
            texture,                               // destinationTexture
            0,                                     // destinationSlice
            0,                                     // destinationLevel
            MTL::Origin::Make(0, 0, 0)             // destinationOrigin
        );

        blitEncoder->endEncoding();
        cmdBuffer->commit();
        cmdBuffer->waitUntilCompleted();
    } else {
        // CPU-accessible texture: direct write
        Uint32 bytesPerPixel = 4; // Assume RGBA8 for now
        Uint32 bytesPerRow = texRes.width * bytesPerPixel;

        MTL::Region region(0, 0, 0, texRes.width, texRes.height, 1);
        texture->replaceRegion(region, 0, data, bytesPerRow);
    }
}

BufferHandle RHI_Metal::copySwapchainToBuffer(Uint32& outWidth, Uint32& outHeight) {
    if (!currentDrawable) {
        fmt::print(stderr, "No current drawable for screenshot\n");
        return BufferHandle{};
    }

    MTL::Texture* texture = currentDrawable->texture();
    outWidth = static_cast<Uint32>(texture->width());
    outHeight = static_cast<Uint32>(texture->height());

    Uint32 bytesPerPixel = 4; // RGBA8 or BGRA8
    Uint32 bytesPerRow = outWidth * bytesPerPixel;
    size_t bufferSize = bytesPerRow * outHeight;

    // Create CPU-readable buffer (Shared storage mode)
    auto buffer = NS::TransferPtr(device->newBuffer(bufferSize, MTL::ResourceStorageModeShared));
    if (!buffer) {
        fmt::print(stderr, "Failed to create screenshot buffer\n");
        return BufferHandle{};
    }

    // A command buffer may only have one active encoder at a time. End any
    // render/compute encoder still open before creating the blit encoder,
    // otherwise Metal raises a validation assertion (crash).
    if (currentRenderEncoder) {
        currentRenderEncoder->endEncoding();
        currentRenderEncoder = nullptr;
    }
    if (currentComputeEncoder) {
        currentComputeEncoder->endEncoding();
        currentComputeEncoder = nullptr;
    }

    // Create blit encoder to copy texture to buffer
    auto blitEncoder = currentCommandBuffer->blitCommandEncoder();
    if (!blitEncoder) {
        fmt::print(stderr, "Failed to create blit encoder for screenshot\n");
        return BufferHandle{};
    }

    // Copy texture to buffer
    blitEncoder->copyFromTexture(
        texture,                               // sourceTexture
        0,                                     // sourceSlice
        0,                                     // sourceLevel
        MTL::Origin::Make(0, 0, 0),           // sourceOrigin
        MTL::Size::Make(outWidth, outHeight, 1), // sourceSize
        buffer.get(),                         // destinationBuffer
        0,                                     // destinationOffset
        bytesPerRow,                          // destinationBytesPerRow
        bytesPerRow * outHeight               // destinationBytesPerImage
    );

    blitEncoder->endEncoding();

    // Store in buffers map
    Uint32 id = nextBufferId++;
    buffers[id] = {buffer, bufferSize, false, nullptr};

    return BufferHandle{id};
}

void* RHI_Metal::mapBuffer(BufferHandle handle) {
    auto it = buffers.find(handle.id);
    if (it == buffers.end()) {
        return nullptr;
    }

    BufferResource& bufferRes = it->second;
    if (bufferRes.isMapped) {
        return bufferRes.mappedPointer;
    }

    // For Metal, buffers with Shared storage mode are always CPU-accessible
    void* contents = bufferRes.buffer->contents();
    if (contents) {
        bufferRes.isMapped = true;
        bufferRes.mappedPointer = contents;
    }

    return contents;
}

void RHI_Metal::unmapBuffer(BufferHandle handle) {
    auto it = buffers.find(handle.id);
    if (it == buffers.end()) {
        return;
    }

    // For Metal Shared storage, no explicit unmap needed
    // Just mark as unmapped for consistency
    it->second.isMapped = false;
    it->second.mappedPointer = nullptr;
}

// ============================================================================
// Frame Operations
// ============================================================================

void RHI_Metal::beginFrame() {
    // Get next drawable. This legitimately fails when the window is occluded
    // or minimized — skip the frame instead of crashing; endFrame() and every
    // command-recording call no-op while currentCommandBuffer is null.
    currentDrawable = swapchain->nextDrawable();
    if (!currentDrawable) {
        currentCommandBuffer = nullptr;
        return;
    }

    // Create command buffer
    currentCommandBuffer = NS::TransferPtr(commandQueue->commandBuffer());
    if (!currentCommandBuffer) {
        throw std::runtime_error("Failed to create command buffer");
    }

    // Latch GPU timing for this frame (the flag may be toggled mid-frame from
    // the UI; slot bookkeeping must stay consistent within a frame).
    gpuTimingActiveThisFrame = gpuTimingEnabled && gpuTimingSupported;
    nextTimingSlot = 0;
    framePassSamples.clear();
}

void RHI_Metal::endFrame() {
    // Frame was skipped (no drawable available in beginFrame)
    if (!currentCommandBuffer) {
        currentDrawable = nullptr;
        return;
    }

    // End any active encoder
    if (currentRenderEncoder) {
        currentRenderEncoder->endEncoding();
        currentRenderEncoder = nullptr;
    }
    if (currentComputeEncoder) {
        currentComputeEncoder->endEncoding();
        currentComputeEncoder = nullptr;
    }

    // Schedule GPU timing resolution for when this command buffer completes
    resolveGpuTimings();

    // Present drawable
    if (currentDrawable) {
        currentCommandBuffer->presentDrawable(currentDrawable);
    }

    // Commit command buffer
    currentCommandBuffer->commit();

    // Reset frame state
    currentDrawable = nullptr;
    currentCommandBuffer = nullptr;
}

void RHI_Metal::beginRenderPass(const RenderPassDesc& desc) {
    if (!currentCommandBuffer) {
        return;  // frame was skipped
    }

    // End any existing encoder
    if (currentRenderEncoder) {
        currentRenderEncoder->endEncoding();
        currentRenderEncoder = nullptr;
    }

    // Create render pass descriptor
    auto renderPassDesc = MTL::RenderPassDescriptor::alloc()->init();

    // Setup color attachments
    for (size_t i = 0; i < desc.colorAttachments.size(); i++) {
        auto colorAttachment = renderPassDesc->colorAttachments()->object(i);

        // Use swapchain or custom texture
        if (desc.colorAttachments[i].id == 0 && currentDrawable) {
            colorAttachment->setTexture(currentDrawable->texture());
        } else {
            auto it = textures.find(desc.colorAttachments[i].id);
            if (it == textures.end()) {
                renderPassDesc->release();
                throw std::runtime_error(fmt::format("Failed to find color attachment texture with id {}", desc.colorAttachments[i].id));
            }
            colorAttachment->setTexture(it->second.texture.get());
        }

        // Load/store operations
        if (i < desc.loadColor.size() && desc.loadColor[i]) {
            colorAttachment->setLoadAction(MTL::LoadActionLoad);
        } else {
            colorAttachment->setLoadAction(MTL::LoadActionClear);
            if (i < desc.clearColors.size()) {
                auto& clearColor = desc.clearColors[i];
                MTL::ClearColor cc(clearColor.r, clearColor.g, clearColor.b, clearColor.a);
                colorAttachment->setClearColor(cc);
            }
        }

        // Optional MSAA resolve target for this attachment
        if (i < desc.resolveAttachments.size() && desc.resolveAttachments[i].isValid()) {
            auto resolveIt = textures.find(desc.resolveAttachments[i].id);
            if (resolveIt == textures.end()) {
                renderPassDesc->release();
                throw std::runtime_error(fmt::format("Failed to find resolve attachment texture with id {}", desc.resolveAttachments[i].id));
            }
            colorAttachment->setResolveTexture(resolveIt->second.texture.get());
            colorAttachment->setStoreAction(MTL::StoreActionStoreAndMultisampleResolve);
        } else {
            colorAttachment->setStoreAction(MTL::StoreActionStore);
        }
    }

    // Setup depth attachment. An invalid handle means "no depth".
    // (Handle id 0 is the swapchain sentinel and never a depth texture.)
    if (desc.depthAttachment.isValid() && desc.depthAttachment.id != 0) {
        auto depthAttachment = renderPassDesc->depthAttachment();
        auto it = textures.find(desc.depthAttachment.id);
        if (it == textures.end()) {
            renderPassDesc->release();
            throw std::runtime_error(fmt::format("Failed to find depth attachment texture with id {}", desc.depthAttachment.id));
        }
        depthAttachment->setTexture(it->second.texture.get());

        if (desc.loadDepth) {
            depthAttachment->setLoadAction(MTL::LoadActionLoad);
        } else {
            depthAttachment->setLoadAction(MTL::LoadActionClear);
            depthAttachment->setClearDepth(desc.clearDepth);
        }
        depthAttachment->setStoreAction(MTL::StoreActionStore);
    }

    // Attach GPU timestamp sampling for this pass (no-op when timing is off)
    NS::UInteger timingBegin, timingEnd;
    if (allocateTimingSlots(desc.name, timingBegin, timingEnd)) {
        auto* sampleAttachment = renderPassDesc->sampleBufferAttachments()->object(0);
        sampleAttachment->setSampleBuffer(gpuTimerSampleBuffer.get());
        sampleAttachment->setStartOfVertexSampleIndex(timingBegin);
        sampleAttachment->setEndOfFragmentSampleIndex(timingEnd);
    }

    // Create render command encoder
    currentRenderEncoder = currentCommandBuffer->renderCommandEncoder(renderPassDesc);
    renderPassDesc->release();

    if (!currentRenderEncoder) {
        throw std::runtime_error("Failed to create render command encoder");
    }

    // Set viewport and scissor to the actual attachment size (falls back to
    // the swapchain size when the first color attachment is the drawable).
    Uint32 passWidth = swapchainWidth;
    Uint32 passHeight = swapchainHeight;
    if (!desc.colorAttachments.empty() && desc.colorAttachments[0].id != 0) {
        auto it = textures.find(desc.colorAttachments[0].id);
        if (it != textures.end()) {
            passWidth = it->second.width;
            passHeight = it->second.height;
        }
    }

    MTL::Viewport viewport;
    viewport.originX = 0.0;
    viewport.originY = 0.0;
    viewport.width = static_cast<double>(passWidth);
    viewport.height = static_cast<double>(passHeight);
    viewport.znear = 0.0;
    viewport.zfar = 1.0;
    currentRenderEncoder->setViewport(viewport);

    MTL::ScissorRect scissorRect;
    scissorRect.x = 0;
    scissorRect.y = 0;
    scissorRect.width = passWidth;
    scissorRect.height = passHeight;
    currentRenderEncoder->setScissorRect(scissorRect);
}

void RHI_Metal::endRenderPass() {
    if (currentRenderEncoder) {
        currentRenderEncoder->endEncoding();
        currentRenderEncoder = nullptr;
    }
}

// ============================================================================
// Rendering Commands
// ============================================================================

void RHI_Metal::bindPipeline(PipelineHandle pipeline) {
    currentPipeline = pipeline;

    auto it = pipelines.find(pipeline.id);
    if (it != pipelines.end() && currentRenderEncoder && it->second.renderPipeline) {
        const PipelineResource& res = it->second;
        currentRenderEncoder->setRenderPipelineState(res.renderPipeline.get());

        // Apply the fixed-function state captured from PipelineDesc
        currentRenderEncoder->setCullMode(res.cullMode);
        currentRenderEncoder->setFrontFacingWinding(res.winding);
        if (res.depthStencilState) {
            currentRenderEncoder->setDepthStencilState(res.depthStencilState.get());
        }
        currentPrimitiveType = res.primitiveType;
    }
}

void RHI_Metal::bindVertexBuffer(BufferHandle buffer, Uint32 binding, size_t offset) {
    currentVertexBuffer = buffer;

    auto it = buffers.find(buffer.id);
    if (it != buffers.end() && currentRenderEncoder) {
        currentRenderEncoder->setVertexBuffer(it->second.buffer.get(), offset, binding);
    }
}

void RHI_Metal::bindIndexBuffer(BufferHandle buffer, size_t offset) {
    currentIndexBuffer = buffer;
    // Metal binds index buffer in drawIndexed call
}

void RHI_Metal::setUniformBuffer(Uint32 set, Uint32 binding, BufferHandle buffer, size_t offset, size_t range) {
    auto it = buffers.find(buffer.id);
    if (it != buffers.end() && currentRenderEncoder) {
        currentRenderEncoder->setVertexBuffer(it->second.buffer.get(), offset, binding);
        currentRenderEncoder->setFragmentBuffer(it->second.buffer.get(), offset, binding);
    }
}

void RHI_Metal::setStorageBuffer(Uint32 set, Uint32 binding, BufferHandle buffer, size_t offset, size_t range) {
    auto it = buffers.find(buffer.id);
    if (it != buffers.end() && currentRenderEncoder) {
        currentRenderEncoder->setVertexBuffer(it->second.buffer.get(), offset, binding);
        currentRenderEncoder->setFragmentBuffer(it->second.buffer.get(), offset, binding);
    }
}

void RHI_Metal::setVertexBuffer(Uint32 binding, BufferHandle buffer, size_t offset, size_t range) {
    auto it = buffers.find(buffer.id);
    if (it != buffers.end() && currentRenderEncoder) {
        currentRenderEncoder->setVertexBuffer(it->second.buffer.get(), offset, binding);
    }
}

void RHI_Metal::setFragmentBuffer(Uint32 binding, BufferHandle buffer, size_t offset, size_t range) {
    auto it = buffers.find(buffer.id);
    if (it != buffers.end() && currentRenderEncoder) {
        currentRenderEncoder->setFragmentBuffer(it->second.buffer.get(), offset, binding);
    }
}

void RHI_Metal::setTexture(Uint32 set, Uint32 binding, TextureHandle texture, SamplerHandle sampler) {
    auto texIt = textures.find(texture.id);
    auto samplerIt = samplers.find(sampler.id);

    if (texIt != textures.end() && currentRenderEncoder) {
        currentRenderEncoder->setFragmentTexture(texIt->second.texture.get(), binding);
    }

    if (samplerIt != samplers.end() && currentRenderEncoder) {
        currentRenderEncoder->setFragmentSamplerState(samplerIt->second.sampler.get(), binding);
    }
}

void RHI_Metal::setVertexBytes(const void* data, size_t size, Uint32 binding) {
    if (currentRenderEncoder && data && size > 0) {
        currentRenderEncoder->setVertexBytes(data, size, binding);
    }
}

void RHI_Metal::setFragmentBytes(const void* data, size_t size, Uint32 binding) {
    if (currentRenderEncoder && data && size > 0) {
        currentRenderEncoder->setFragmentBytes(data, size, binding);
    }
}

void RHI_Metal::draw(Uint32 vertexCount, Uint32 instanceCount, Uint32 firstVertex, Uint32 firstInstance) {
    if (currentRenderEncoder) {
        currentRenderEncoder->drawPrimitives(
            currentPrimitiveType,
            firstVertex,
            vertexCount,
            instanceCount,
            firstInstance
        );
    }
}

void RHI_Metal::drawIndexed(Uint32 indexCount, Uint32 instanceCount, Uint32 firstIndex, int32_t vertexOffset, Uint32 firstInstance) {
    auto it = buffers.find(currentIndexBuffer.id);
    if (it != buffers.end() && currentRenderEncoder) {
        currentRenderEncoder->drawIndexedPrimitives(
            currentPrimitiveType,
            indexCount,
            MTL::IndexTypeUInt32,
            it->second.buffer.get(),
            firstIndex * sizeof(Uint32),
            instanceCount,
            vertexOffset,
            firstInstance
        );
    }
}

// ============================================================================
// Utility
// ============================================================================

Uint32 RHI_Metal::getSwapchainWidth() const {
    return swapchainWidth;
}

Uint32 RHI_Metal::getSwapchainHeight() const {
    return swapchainHeight;
}

PixelFormat RHI_Metal::getSwapchainFormat() const {
    // Must match the format configured on the CAMetalLayer in initialize()
    return PixelFormat::RGBA8_SRGB;
}

// ============================================================================
// Backend Query Interface
// ============================================================================

void* RHI_Metal::getBackendDevice() const {
    return (void*)device;
}

void* RHI_Metal::getBackendQueue() const {
    return (void*)commandQueue.get();
}

void* RHI_Metal::getBackendCommandBuffer() const {
    return (void*)currentCommandBuffer.get();
}

MTL::RenderCommandEncoder* RHI_Metal::getCurrentRenderEncoder() const {
    return currentRenderEncoder;
}

CA::MetalDrawable* RHI_Metal::getCurrentDrawable() const {
    return currentDrawable;
}

// ============================================================================
// Format Conversion Helpers
// ============================================================================

MTL::PixelFormat RHI_Metal::convertPixelFormat(PixelFormat format) {
    switch (format) {
        case PixelFormat::RGBA8_UNORM: return MTL::PixelFormatRGBA8Unorm;
        case PixelFormat::RGBA8_SRGB: return MTL::PixelFormatRGBA8Unorm_sRGB;
        case PixelFormat::BGRA8_UNORM: return MTL::PixelFormatBGRA8Unorm;
        case PixelFormat::BGRA8_SRGB: return MTL::PixelFormatBGRA8Unorm_sRGB;
        case PixelFormat::RGBA16_FLOAT: return MTL::PixelFormatRGBA16Float;
        case PixelFormat::RGBA32_FLOAT: return MTL::PixelFormatRGBA32Float;
        case PixelFormat::R8_UNORM: return MTL::PixelFormatR8Unorm;
        case PixelFormat::R16_FLOAT: return MTL::PixelFormatR16Float;
        case PixelFormat::R32_FLOAT: return MTL::PixelFormatR32Float;
        case PixelFormat::RG32_FLOAT: return MTL::PixelFormatRG32Float;
        // RGB32 has no Metal texture equivalent (vertex-attribute-only format)
        case PixelFormat::RGB32_FLOAT: return MTL::PixelFormatRGBA32Float;
        case PixelFormat::Depth32Float: return MTL::PixelFormatDepth32Float;
        case PixelFormat::Depth24Stencil8: return MTL::PixelFormatDepth24Unorm_Stencil8;
        default: return MTL::PixelFormatRGBA8Unorm;
    }
}

MTL::TextureUsage RHI_Metal::convertTextureUsage(TextureUsage usage) {
    MTL::TextureUsage metalUsage = MTL::TextureUsageUnknown;

    if (hasUsage(usage, TextureUsage::Sampled)) {
        metalUsage |= MTL::TextureUsageShaderRead;
    }
    if (hasUsage(usage, TextureUsage::Storage)) {
        // Storage textures are read-write in compute shaders
        metalUsage |= MTL::TextureUsageShaderRead | MTL::TextureUsageShaderWrite;
    }
    if (hasUsage(usage, TextureUsage::RenderTarget)) {
        metalUsage |= MTL::TextureUsageRenderTarget;
    }
    if (hasUsage(usage, TextureUsage::DepthStencil)) {
        metalUsage |= MTL::TextureUsageRenderTarget;
    }

    return metalUsage;
}

MTL::SamplerAddressMode RHI_Metal::convertSamplerAddressMode(AddressMode mode) {
    switch (mode) {
        case AddressMode::Repeat: return MTL::SamplerAddressModeRepeat;
        case AddressMode::MirrorRepeat: return MTL::SamplerAddressModeMirrorRepeat;
        case AddressMode::ClampToEdge: return MTL::SamplerAddressModeClampToEdge;
        case AddressMode::ClampToBorder: return MTL::SamplerAddressModeClampToZero;
        default: return MTL::SamplerAddressModeRepeat;
    }
}

MTL::SamplerMinMagFilter RHI_Metal::convertSamplerFilter(FilterMode filter) {
    switch (filter) {
        case FilterMode::Nearest: return MTL::SamplerMinMagFilterNearest;
        case FilterMode::Linear: return MTL::SamplerMinMagFilterLinear;
        default: return MTL::SamplerMinMagFilterLinear;
    }
}

MTL::SamplerMipFilter RHI_Metal::convertSamplerMipFilter(FilterMode filter) {
    switch (filter) {
        case FilterMode::Nearest: return MTL::SamplerMipFilterNearest;
        case FilterMode::Linear: return MTL::SamplerMipFilterLinear;
        default: return MTL::SamplerMipFilterLinear;
    }
}

MTL::CompareFunction RHI_Metal::convertCompareOp(CompareOp op) {
    switch (op) {
        case CompareOp::Never: return MTL::CompareFunctionNever;
        case CompareOp::Less: return MTL::CompareFunctionLess;
        case CompareOp::Equal: return MTL::CompareFunctionEqual;
        case CompareOp::LessOrEqual: return MTL::CompareFunctionLessEqual;
        case CompareOp::Greater: return MTL::CompareFunctionGreater;
        case CompareOp::NotEqual: return MTL::CompareFunctionNotEqual;
        case CompareOp::GreaterOrEqual: return MTL::CompareFunctionGreaterEqual;
        case CompareOp::Always: return MTL::CompareFunctionAlways;
        default: return MTL::CompareFunctionLess;
    }
}

MTL::PrimitiveType RHI_Metal::convertPrimitiveTopology(PrimitiveTopology topology) {
    switch (topology) {
        case PrimitiveTopology::PointList: return MTL::PrimitiveTypePoint;
        case PrimitiveTopology::LineList: return MTL::PrimitiveTypeLine;
        case PrimitiveTopology::LineStrip: return MTL::PrimitiveTypeLineStrip;
        case PrimitiveTopology::TriangleList: return MTL::PrimitiveTypeTriangle;
        case PrimitiveTopology::TriangleStrip: return MTL::PrimitiveTypeTriangleStrip;
        default: return MTL::PrimitiveTypeTriangle;
    }
}

MTL::CullMode RHI_Metal::convertCullMode(CullMode mode) {
    switch (mode) {
        case CullMode::None: return MTL::CullModeNone;
        case CullMode::Front: return MTL::CullModeFront;
        case CullMode::Back: return MTL::CullModeBack;
        default: return MTL::CullModeBack;
    }
}

MTL::Winding RHI_Metal::convertFrontFace(bool counterClockwise) {
    return counterClockwise ? MTL::WindingCounterClockwise : MTL::WindingClockwise;
}

// ============================================================================
// Compute Pipeline
// ============================================================================

ComputePipelineHandle RHI_Metal::createComputePipeline(const ComputePipelineDesc& desc) {
    auto it = shaders.find(desc.computeShader.id);
    if (it == shaders.end()) {
        throw std::runtime_error("Invalid compute shader handle");
    }

    NS::Error* error = nullptr;
    auto pipeline = NS::TransferPtr(device->newComputePipelineState(it->second.function.get(), &error));

    if (!pipeline || error) {
        if (error) {
            fmt::print("Compute pipeline creation error: {}\n", error->localizedDescription()->utf8String());
        }
        throw std::runtime_error("Failed to create compute pipeline");
    }

    Uint32 id = nextComputePipelineId++;
    computePipelines[id] = {pipeline};

    return ComputePipelineHandle{id};
}

void RHI_Metal::destroyComputePipeline(ComputePipelineHandle handle) {
    computePipelines.erase(handle.id);
}

// ============================================================================
// Acceleration Structures (Metal Ray Tracing)
// ============================================================================

AccelStructHandle RHI_Metal::createAccelerationStructure(const AccelStructDesc& desc) {
    AccelStructResource resource;
    resource.type = desc.type;
    resource.geometries = desc.geometries;
    resource.instances = desc.instances;

    // Acceleration structure will be built in buildAccelerationStructure()
    resource.accelStruct = nullptr;
    resource.scratchBuffer = nullptr;

    Uint32 id = nextAccelStructId++;
    accelStructs[id] = resource;

    return AccelStructHandle{id};
}

void RHI_Metal::destroyAccelerationStructure(AccelStructHandle handle) {
    accelStructs.erase(handle.id);
}

void RHI_Metal::buildAccelerationStructure(AccelStructHandle handle) {
    auto it = accelStructs.find(handle.id);
    if (it == accelStructs.end()) {
        return;
    }

    auto& resource = it->second;

    if (resource.type == AccelStructType::BottomLevel) {
        // Build BLAS from geometries
        for (const auto& geom : resource.geometries) {
            auto vertexBufIt = buffers.find(geom.vertexBuffer.id);
            auto indexBufIt = buffers.find(geom.indexBuffer.id);
            if (vertexBufIt == buffers.end() || indexBufIt == buffers.end()) {
                continue;
            }

            auto geomDesc = NS::TransferPtr(MTL::AccelerationStructureTriangleGeometryDescriptor::alloc()->init());
            geomDesc->setVertexBuffer(vertexBufIt->second.buffer.get());
            geomDesc->setVertexBufferOffset(0);
            geomDesc->setVertexStride(geom.vertexStride);
            geomDesc->setIndexBuffer(indexBufIt->second.buffer.get());
            geomDesc->setIndexBufferOffset(0);
            geomDesc->setIndexType(MTL::IndexTypeUInt32);
            geomDesc->setTriangleCount(geom.indexCount / 3);

            auto accelDesc = NS::TransferPtr(MTL::PrimitiveAccelerationStructureDescriptor::alloc()->init());
            NS::Object* geomDescPtr = static_cast<NS::Object*>(geomDesc.get());
            auto geomArray = NS::Array::array(&geomDescPtr, 1);
            accelDesc->setGeometryDescriptors(geomArray);

            auto accelSizes = device->accelerationStructureSizes(accelDesc.get());

            resource.scratchBuffer = NS::TransferPtr(device->newBuffer(accelSizes.buildScratchBufferSize, MTL::ResourceStorageModePrivate));
            resource.accelStruct = NS::TransferPtr(device->newAccelerationStructure(accelSizes.accelerationStructureSize));

            // Need to build via command buffer
            auto cmdBuffer = commandQueue->commandBuffer();
            auto encoder = cmdBuffer->accelerationStructureCommandEncoder();
            encoder->buildAccelerationStructure(resource.accelStruct.get(), accelDesc.get(), resource.scratchBuffer.get(), 0);
            encoder->endEncoding();
            cmdBuffer->commit();
            cmdBuffer->waitUntilCompleted();
        }
    } else {
        // Build TLAS from instances
        // This will be implemented when needed
        fmt::print("Warning: TLAS building not yet fully implemented\n");
    }
}

void RHI_Metal::updateAccelerationStructure(AccelStructHandle handle, const std::vector<AccelStructInstance>& instances) {
    auto it = accelStructs.find(handle.id);
    if (it == accelStructs.end()) {
        return;
    }

    auto& resource = it->second;
    resource.instances = instances;

    // Rebuild TLAS with new instances
    if (resource.type == AccelStructType::TopLevel) {
        buildAccelerationStructure(handle);
    }
}

// ============================================================================
// Compute Commands
// ============================================================================

void RHI_Metal::beginComputePass() {
    // End any active render encoder
    if (currentRenderEncoder) {
        currentRenderEncoder->endEncoding();
        currentRenderEncoder = nullptr;
    }

    // Create compute encoder
    if (currentCommandBuffer && !currentComputeEncoder) {
        NS::UInteger timingBegin, timingEnd;
        if (allocateTimingSlots("Compute", timingBegin, timingEnd)) {
            auto passDesc = NS::TransferPtr(MTL::ComputePassDescriptor::computePassDescriptor());
            auto* sampleAttachment = passDesc->sampleBufferAttachments()->object(0);
            sampleAttachment->setSampleBuffer(gpuTimerSampleBuffer.get());
            sampleAttachment->setStartOfEncoderSampleIndex(timingBegin);
            sampleAttachment->setEndOfEncoderSampleIndex(timingEnd);
            currentComputeEncoder = currentCommandBuffer->computeCommandEncoder(passDesc.get());
        } else {
            currentComputeEncoder = currentCommandBuffer->computeCommandEncoder();
        }
    }
}

void RHI_Metal::endComputePass() {
    if (currentComputeEncoder) {
        currentComputeEncoder->endEncoding();
        currentComputeEncoder = nullptr;
    }
}

void RHI_Metal::bindComputePipeline(ComputePipelineHandle pipeline) {
    currentComputePipeline = pipeline;

    auto it = computePipelines.find(pipeline.id);
    if (it != computePipelines.end() && currentComputeEncoder) {
        currentComputeEncoder->setComputePipelineState(it->second.pipeline.get());
    }
}

void RHI_Metal::setComputeBuffer(Uint32 binding, BufferHandle buffer, size_t offset, size_t range) {
    auto it = buffers.find(buffer.id);
    if (it != buffers.end() && currentComputeEncoder) {
        currentComputeEncoder->setBuffer(it->second.buffer.get(), offset, binding);
    }
}

void RHI_Metal::setComputeTexture(Uint32 binding, TextureHandle texture) {
    auto it = textures.find(texture.id);
    if (it != textures.end() && currentComputeEncoder) {
        currentComputeEncoder->setTexture(it->second.texture.get(), binding);
    }
}

void RHI_Metal::setAccelerationStructure(Uint32 binding, AccelStructHandle accelStruct) {
    auto it = accelStructs.find(accelStruct.id);
    if (it != accelStructs.end() && currentComputeEncoder && it->second.accelStruct) {
        currentComputeEncoder->setAccelerationStructure(it->second.accelStruct.get(), binding);
    }
}

void RHI_Metal::dispatch(Uint32 groupCountX, Uint32 groupCountY, Uint32 groupCountZ) {
    if (currentComputeEncoder) {
        MTL::Size threadgroups(groupCountX, groupCountY, groupCountZ);
        MTL::Size threadsPerGroup(1, 1, 1); // Should be configured based on shader
        currentComputeEncoder->dispatchThreadgroups(threadgroups, threadsPerGroup);
    }
}
