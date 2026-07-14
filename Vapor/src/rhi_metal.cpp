// NOTE: The metal-cpp implementation (*_PRIVATE_IMPLEMENTATION) is emitted by
// exactly ONE translation unit — renderer_metal.cpp. This TU only uses the
// declarations, so it must NOT define those macros (doing so made multiple TUs
// emit the implementation → duplicate symbols at link).
#include "rhi_metal.hpp"
#include "stats_log.hpp"
#include <fmt/core.h>
#include <stdexcept>
#include <algorithm>
#include <cstdio>
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

    // One permit per in-flight frame; beginFrame() waits, the completion handler
    // signals. getMaxFramesInFlight() is the single source of truth (the renderer
    // sizes its per-frame buffer slots off it too), so the CPU is never more than
    // that many frames ahead.
    frameSemaphore = dispatch_semaphore_create(getMaxFramesInFlight());

    // Device capabilities. Raytracing is force-disabled on CI runners: the
    // virtualized GPU advertises support but acceleration-structure builds
    // fail there (matching the old renderer's behavior).
    capabilities.raytracing = device->supportsRaytracing() && !std::getenv("GITHUB_ACTIONS");
    capabilities.computeShaders = true;
    capabilities.gpuTimestamps = gpuTimingSupported;
    // Object/mesh shaders (meshlet path): Apple GPU family 7 (A14/M1) and later,
    // or Mac family 2. Detected here; the pipeline type is wired up in Phase C.
    capabilities.meshShaders = device->supportsFamily(MTL::GPUFamilyApple7) ||
                               device->supportsFamily(MTL::GPUFamilyMac2);
    // Indirect command buffers with inherited pipeline state + argument-buffer
    // texture tables (the ICB draw mode). Same families as mesh shaders — every
    // Apple-silicon Mac qualifies; the inherit-pipeline and Tier-2 argument
    // buffer requirements are both met there.
    capabilities.indirectCommandBuffers = device->supportsFamily(MTL::GPUFamilyApple7) ||
                                          device->supportsFamily(MTL::GPUFamilyMac2);
    // Bindless texture tables ride on Tier-2 argument buffers — same families.
    capabilities.bindlessTextures = capabilities.indirectCommandBuffers;

    // Backend telemetry: one grouped "[MTL]" line per --stats interval. Metal is
    // unified memory, so these counts (plus RSS) are the leak-hunt signal.
    Vapor::StatsLog::get().addSource("MTL", [this](Vapor::StatLine& s) {
        s.add("buf", buffers.size());
        s.add("tex", textures.size());
        s.add("smp", samplers.size());
        s.add("shd", shaders.size());
        s.add("pso", pipelines.size());
        s.add("cpso", computePipelines.size());
        s.add("pendingUpCmds", pendingUploadCmds.size());
        s.add("oversize", oversizeStaging.size());
        s.add("stagingOff_KB", stagingRingOffset / 1024);
        // GPU-timing race telemetry (read-and-reset per interval): frames that
        // skipped timing because their region's handler hadn't run, and the max
        // handler lateness in frames. Both ~0 while spikes persist would rule
        // OUT stale-slot pairing as the spike mechanism.
        s.add("tSkips/int", statsTimingSkips.exchange(0));
        s.add("tLateMax/int", statsHandlerLateMax.exchange(0));
    });

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
            desc->setSampleCount(GPU_TIMER_SAMPLE_COUNT * kTimingRegions);
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

// ============================================================================
// Batched Upload Stream
// ============================================================================

MTL::BlitCommandEncoder* RHI_Metal::ensureUploadBlit() {
    if (!stagingRingBuffer) {
        stagingRingBuffer = NS::TransferPtr(device->newBuffer(STAGING_RING_SIZE, MTL::ResourceStorageModeShared));
    }
    if (!uploadCmdBuffer) {
        // commandBuffer() returns an AUTORELEASED object (+0), so it must be
        // RetainPtr, not TransferPtr — TransferPtr assumes an owned +1 (alloc/
        // new/copy) and under-retains. Harmless while no autorelease pool
        // drained the frame; now that beginFrame/endFrame bracket one, a
        // TransferPtr here would let the pool free a command buffer we still
        // hold in pendingUploadCmds, crashing next frame in ->status().
        uploadCmdBuffer = NS::RetainPtr(commandQueue->commandBuffer());
        uploadBlitEncoder = uploadCmdBuffer->blitCommandEncoder();
        uploadBlitEncoder->setLabel(NS::String::string("Uploads", NS::UTF8StringEncoding));
    }
    return uploadBlitEncoder;
}

void* RHI_Metal::allocStaging(size_t size, size_t& outOffset) {
    size_t aligned = (stagingRingOffset + 15) & ~size_t(15);
    if (aligned + size > STAGING_RING_SIZE) {
        submitUploads(true);  // wraps: wait so the space is reusable
        aligned = 0;
    }
    ensureUploadBlit();  // ring buffer must exist before we hand out pointers
    outOffset = aligned;
    stagingRingOffset = aligned + size;
    return static_cast<char*>(stagingRingBuffer->contents()) + aligned;
}

MTL::Buffer* RHI_Metal::stageData(const void* data, size_t size, size_t& outOffset) {
    if (size <= STAGING_RING_SIZE) {
        void* dst = allocStaging(size, outOffset);
        std::memcpy(dst, data, size);
        return stagingRingBuffer.get();
    }
    // Oversize: dedicated one-shot buffer. It must be kept alive by US until
    // the upload batch completes — the encoder retains buffers at encode
    // time, which happens only after this function returns. (Returning the
    // bare pointer from a temporary SharedPtr here was a use-after-free that
    // crashed inside Metal's copyFromBuffer on real hardware.)
    auto big = NS::TransferPtr(device->newBuffer(size, MTL::ResourceStorageModeShared));
    std::memcpy(big->contents(), data, size);
    outOffset = 0;
    ensureUploadBlit();
    oversizeStaging.push_back(big);
    return big.get();
}

void RHI_Metal::submitUploads(bool waitForCompletion) {
    if (uploadCmdBuffer) {
        uploadBlitEncoder->endEncoding();
        uploadBlitEncoder = nullptr;
        uploadCmdBuffer->commit();
        pendingUploadCmds.push_back(uploadCmdBuffer);
        uploadCmdBuffer = nullptr;
    }
    if (waitForCompletion) {
        for (auto& cmd : pendingUploadCmds) {
            cmd->waitUntilCompleted();
        }
        pendingUploadCmds.clear();
        oversizeStaging.clear();
        stagingRingOffset = 0;
    }
}

void RHI_Metal::flushUploads() {
    submitUploads(true);
}

bool RHI_Metal::allocateTimingSlots(const char* passName, NS::UInteger& outBegin, NS::UInteger& outEnd) {
    if (!gpuTimingActiveThisFrame || !gpuTimerSampleBuffer) {
        return false;
    }
    if (nextTimingSlot + 2 > timingBaseSlot + GPU_TIMER_SAMPLE_COUNT) {
        return false;  // this region's per-frame slot budget exhausted; pass goes untimed
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
    // Resolve ONLY this frame's region [base, base+count) and index relative to
    // it. Other in-flight frames own other regions and may be writing them right
    // now — we never read those slots, so there is no cross-frame race.
    NS::UInteger base = capturedInfo.front().beginIdx;
    NS::UInteger count = capturedInfo.back().endIdx + 1 - base;
    // Gate this region against reuse until the handler below has read it, and
    // stamp the registration frame so handler lateness is measurable.
    const NS::UInteger region = timingBaseSlot / GPU_TIMER_SAMPLE_COUNT;
    timingRegionBusy[region].store(true, std::memory_order_release);
    const uint32_t registeredFrame = timingFrameCounter;
    currentCommandBuffer->addCompletedHandler([this, capturedInfo, capturedBuf, base, count,
                                               region, registeredFrame](MTL::CommandBuffer*) {
        NS::Data* data = capturedBuf->resolveCounterRange(NS::Range::Make(base, count));
        if (data) {
            auto* timestamps = reinterpret_cast<const MTL::CounterResultTimestamp*>(data->mutableBytes());
            // Spike capture: a >50ms single-pass window is a real wall-clock gap
            // inside that pass (drawable/present back-pressure, GPU preemption) —
            // region reuse is completion-gated, so it can't be cross-frame
            // pollution. Print the first few spikes' begin/end offsets relative
            // to the frame's first sample so the mechanism is identifiable.
            const uint64_t firstBegin = timestamps[capturedInfo.front().beginIdx - base].timestamp;
            uint64_t minBegin = ~0ull;
            uint64_t maxEnd = 0;
            char spikeBuf[512];
            int spikeLen = 0;
            std::vector<std::pair<uint64_t, uint64_t>> windows;
            windows.reserve(capturedInfo.size());
            {
                std::lock_guard<std::mutex> lock(gpuTimingMutex);
                gpuPassTimings.clear();
                gpuPassTimings.reserve(capturedInfo.size());
                for (const auto& info : capturedInfo) {
                    uint64_t begin = timestamps[info.beginIdx - base].timestamp;
                    uint64_t end = timestamps[info.endIdx - base].timestamp;
                    // Validity guards: 0 / ~0 means the GPU never wrote the
                    // sample (encoder type without this stage boundary); a delta
                    // over a second is a stale slot, not a timing.
                    bool beginValid = begin != 0 && begin != ~0ull;
                    bool endValid = end != 0 && end != ~0ull;
                    double ms = (beginValid && endValid && end >= begin)
                        ? static_cast<double>(end - begin) / 1e6 : 0.0;
                    if (ms > 1000.0) ms = 0.0;
                    // Per-pass = its own fragment/encoder window (EoF - SoF).
                    gpuPassTimings.push_back({info.name, ms});
                    if (beginValid && endValid && end > begin && ms <= 1000.0) {
                        windows.emplace_back(begin, end);
                        minBegin = std::min(minBegin, begin);
                        maxEnd = std::max(maxEnd, end);
                    }
                    if (ms > 50.0 && spikeLen < static_cast<int>(sizeof(spikeBuf)) - 96) {
                        spikeLen += snprintf(spikeBuf + spikeLen, sizeof(spikeBuf) - spikeLen,
                                             " %s=%.1fms(b%+.1f,e%+.1f)", info.name.c_str(), ms,
                                             static_cast<double>(static_cast<int64_t>(begin - firstBegin)) / 1e6,
                                             static_cast<double>(static_cast<int64_t>(end - firstBegin)) / 1e6);
                    }
                }
                // span = min sample .. max sample: per-frame LATENCY (pipelined
                // frames overlap, so it may exceed the frame period — healthy).
                gpuFrameSpanMs = (maxEnd > minBegin && minBegin != ~0ull)
                    ? static_cast<double>(maxEnd - minBegin) / 1e6 : 0.0;
                // busy = interval union of the fragment/encoder windows:
                // occupancy, ~frame period when GPU-bound. Fragment windows
                // serialize per pass on TBDR, so this excludes inter-pass idle
                // (present waits) that inflates span.
                std::sort(windows.begin(), windows.end());
                uint64_t busy = 0, curB = 0, curE = 0;
                for (const auto& [b, e] : windows) {
                    if (b > curE) { busy += curE - curB; curB = b; curE = e; }
                    else if (e > curE) { curE = e; }
                }
                busy += curE - curB;
                gpuFrameBusyMs = static_cast<double>(busy) / 1e6;
            }
            if (spikeLen > 0 && gpuSpikeReportsLeft.fetch_sub(1, std::memory_order_relaxed) > 0) {
                fprintf(stderr, "[GPUT] f=%u late=%u span=%.1fms spikes:%s\n",
                        registeredFrame,
                        latestTimingFrame.load(std::memory_order_relaxed) - registeredFrame,
                        static_cast<double>(static_cast<int64_t>(maxEnd - firstBegin)) / 1e6,
                        spikeBuf);
                fflush(stderr);
            }
        }
        // Race telemetry: how many frames after registration did this handler
        // run? >= kTimingRegions would have raced the old rotation-only scheme.
        uint32_t late = latestTimingFrame.load(std::memory_order_relaxed) - registeredFrame;
        uint32_t prev = statsHandlerLateMax.load(std::memory_order_relaxed);
        while (late > prev &&
               !statsHandlerLateMax.compare_exchange_weak(prev, late, std::memory_order_relaxed)) {}
        // ALWAYS free the region (also on the !data path) or timing wedges.
        timingRegionBusy[region].store(false, std::memory_order_release);
    });
}

std::vector<GpuPassTiming> RHI_Metal::getGpuPassTimings() {
    std::lock_guard<std::mutex> lock(gpuTimingMutex);
    return gpuPassTimings;
}

double RHI_Metal::getGpuFrameSpanMs() {
    std::lock_guard<std::mutex> lock(gpuTimingMutex);
    return gpuFrameSpanMs;
}

double RHI_Metal::getGpuFrameBusyMs() {
    std::lock_guard<std::mutex> lock(gpuTimingMutex);
    return gpuFrameBusyMs;
}

void RHI_Metal::shutdown() {
    if (renderer) {
        // Flush and drain outstanding uploads, then wait for the GPU
        submitUploads(true);
        stagingRingBuffer = nullptr;
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

    // waitIdle() above drained the GPU, so no completion handler is still pending
    // to signal this. dispatch objects are ARC/GCD ref-counted; release our ref.
    if (frameSemaphore) {
        // Restore to full count first so the release doesn't assert on a
        // semaphore destroyed while its value is below its initial value.
        for (Uint32 i = 0; i < getMaxFramesInFlight(); i++) {
            dispatch_semaphore_signal(frameSemaphore);
        }
        dispatch_release(frameSemaphore);
        frameSemaphore = nullptr;
    }
    frameSemaphoreAcquired = false;

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

    // Texture type: MSAA / cube / 2D array / plain 2D
    if (desc.sampleCount > 1) {
        textureDesc->setTextureType(MTL::TextureType2DMultisample);
    } else if (desc.isCube) {
        textureDesc->setTextureType(MTL::TextureTypeCube);
    } else if (desc.arrayLayers > 1) {
        textureDesc->setTextureType(MTL::TextureType2DArray);
    } else {
        textureDesc->setTextureType(MTL::TextureType2D);
    }

    textureDesc->setWidth(desc.width);
    textureDesc->setHeight(desc.height);
    textureDesc->setDepth(desc.depth);
    textureDesc->setMipmapLevelCount(desc.mipLevels);
    // Metal: cube textures use arrayLength 1 (the 6 faces are implicit)
    textureDesc->setArrayLength(desc.isCube ? 1 : desc.arrayLayers);
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
        pixelFormatBytesPerPixel(desc.format),
        convertPixelFormat(desc.format)
    };

    return TextureHandle{id};
}

TextureHandle RHI_Metal::createTextureView(const TextureViewDesc& desc) {
    auto it = textures.find(desc.source.id);
    if (it == textures.end() || !it->second.texture) return {};
    MTL::Texture* src = it->second.texture.get();

    MTL::TextureSwizzleChannels sw = (desc.swizzle == TextureSwizzle::RRR1)
        ? MTL::TextureSwizzleChannels::Make(MTL::TextureSwizzleRed, MTL::TextureSwizzleRed,
                                            MTL::TextureSwizzleRed, MTL::TextureSwizzleOne)
        : MTL::TextureSwizzleChannels::Make(MTL::TextureSwizzleRed, MTL::TextureSwizzleGreen,
                                            MTL::TextureSwizzleBlue, MTL::TextureSwizzleAlpha);

    // newTextureView returns an owned (+1) object; TransferPtr is correct. The
    // view retains the source's storage, so the source stays alive on its own.
    auto view = NS::TransferPtr(src->newTextureView(
        src->pixelFormat(), MTL::TextureType2D,
        NS::Range::Make(0, 1),                                    // one mip
        NS::Range::Make(desc.baseArrayLayer, desc.layerCount),   // one array slice
        sw));
    if (!view) return {};

    Uint32 id = nextTextureId++;
    TextureResource r{};
    r.texture = view;
    r.width = it->second.width;
    r.height = it->second.height;
    r.depth = 1;
    r.mipLevels = 1;
    r.bytesPerPixel = it->second.bytesPerPixel;
    r.format = src->pixelFormat();  // the view keeps the source's format
    textures[id] = r;
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
        auto entryName = NS::String::string(entryPoint.c_str(), NS::UTF8StringEncoding);
        if (desc.bindlessMaterials) {
            // Specialize with kBindlessMaterials=true (function constant 0):
            // the PBR fragment reads material textures from the argument table
            // at buffer(13). Shaders never specialized this way see the
            // constant as undefined -> false (is_function_constant_defined).
            auto constants = NS::TransferPtr(MTL::FunctionConstantValues::alloc()->init());
            bool bindless = true;
            constants->setConstantValue(&bindless, MTL::DataTypeBool, NS::UInteger(0));
            NS::Error* fcError = nullptr;
            function = NS::TransferPtr(library->newFunction(entryName, constants.get(), &fcError));
            if (!function || fcError) {
                if (fcError) {
                    fmt::print("Shader specialization error: {}\n",
                               fcError->localizedDescription()->utf8String());
                }
                throw std::runtime_error(fmt::format("Failed to specialize shader entry point '{}'", entryPoint));
            }
        } else {
            function = NS::TransferPtr(library->newFunction(entryName));
        }
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

    // Color attachments: formats are baked into the PSO and must match the
    // render pass (PixelFormat::Swapchain resolves to the layer's format).
    // The same blend mode is applied to every attachment. An EMPTY format
    // list means a depth-only pipeline (e.g. shadow cascades) — declaring a
    // default swapchain attachment here made the debug layer assert when the
    // pass has no color texture.
    for (size_t i = 0; i < desc.colorAttachmentFormats.size(); i++) {
        auto attachment = pipelineDesc->colorAttachments()->object(i);
        PixelFormat fmt = desc.colorAttachmentFormats[i];
        attachment->setPixelFormat(fmt == PixelFormat::Swapchain ? swapchainFormat
                                                                 : convertPixelFormat(fmt));
    }
    // Blending (only meaningful when a color attachment exists).
    if (!desc.colorAttachmentFormats.empty()) {
        auto colorAttachment = pipelineDesc->colorAttachments()->object(0);
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
    }

    // Depth attachment format must match the render pass this pipeline is
    // used in, regardless of whether depth testing is enabled.
    if (desc.hasDepthAttachment) {
        pipelineDesc->setDepthAttachmentPixelFormat(convertPixelFormat(desc.depthAttachmentFormat));
    }

    // Sample count
    pipelineDesc->setSampleCount(desc.sampleCount);

    // Indirect-command-buffer replay (executeICB) requires opting the PSO in.
    if (desc.supportsICB) {
        pipelineDesc->setSupportIndirectCommandBuffers(true);
    }

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

PipelineHandle RHI_Metal::createMeshPipeline(const MeshPipelineDesc& desc) {
    if (!capabilities.meshShaders) return {};
    auto msIt = shaders.find(desc.meshShader.id);
    auto fsIt = shaders.find(desc.fragmentShader.id);
    if (msIt == shaders.end() || fsIt == shaders.end()) {
        throw std::runtime_error("Invalid shader handles for mesh pipeline");
    }

    auto pipelineDesc = MTL::MeshRenderPipelineDescriptor::alloc()->init();
    auto tsIt = shaders.find(desc.taskShader.id);
    if (desc.taskShader.isValid() && tsIt != shaders.end()) {
        pipelineDesc->setObjectFunction(tsIt->second.function.get());
    }
    pipelineDesc->setMeshFunction(msIt->second.function.get());
    pipelineDesc->setFragmentFunction(fsIt->second.function.get());

    // Object/mesh amplification limits. Without these Metal allocates no payload
    // memory and caps the mesh grid at 0 — the object shader runs but emits no
    // mesh threadgroups, so nothing is drawn.
    pipelineDesc->setMaxTotalThreadsPerObjectThreadgroup(desc.taskThreadgroupSize);
    pipelineDesc->setMaxTotalThreadsPerMeshThreadgroup(desc.meshThreadgroupSize);
    pipelineDesc->setMaxTotalThreadgroupsPerMeshGrid(desc.maxMeshThreadgroupsPerObject);
    pipelineDesc->setPayloadMemoryLength(desc.payloadBytes);

    for (size_t i = 0; i < desc.colorAttachmentFormats.size(); i++) {
        auto attachment = pipelineDesc->colorAttachments()->object(i);
        PixelFormat fmt = desc.colorAttachmentFormats[i];
        attachment->setPixelFormat(fmt == PixelFormat::Swapchain ? swapchainFormat
                                                                 : convertPixelFormat(fmt));
        attachment->setBlendingEnabled(false);  // meshlet path draws opaque
    }
    if (desc.hasDepthAttachment) {
        pipelineDesc->setDepthAttachmentPixelFormat(convertPixelFormat(desc.depthAttachmentFormat));
    }
    pipelineDesc->setRasterSampleCount(desc.sampleCount);

    NS::Error* error = nullptr;
    auto pipeline = NS::TransferPtr(device->newRenderPipelineState(
        pipelineDesc, MTL::PipelineOptionNone, nullptr, &error));
    pipelineDesc->release();
    if (!pipeline || error) {
        if (error) {
            fmt::print("Mesh pipeline creation error: {}\n",
                       error->localizedDescription()->utf8String());
        }
        throw std::runtime_error("Failed to create mesh render pipeline");
    }

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
    resource.isMesh = true;
    resource.taskThreads = desc.taskThreadgroupSize;
    resource.meshThreads = desc.meshThreadgroupSize;
    resource.depthStencilState = depthStencilState;
    resource.cullMode = convertCullMode(desc.cullMode);
    resource.winding = convertFrontFace(desc.frontFaceCounterClockwise);
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
        // GPU-only buffer: copy through the staging ring into the batched
        // upload stream (committed at beginFrame / flushUploads() / wrap)
        size_t srcOffset;
        MTL::Buffer* srcBuf = stageData(data, size, srcOffset);
        ensureUploadBlit()->copyFromBuffer(srcBuf, srcOffset, buffer, offset, size);
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

void RHI_Metal::updateTexture(TextureHandle handle, const void* data, size_t size,
                              Uint32 mipLevel, Uint32 arrayLayer) {
    auto it = textures.find(handle.id);
    if (it == textures.end()) {
        return;
    }

    const TextureResource& texRes = it->second;
    MTL::Texture* texture = texRes.texture.get();

    Uint32 mipWidth = std::max(1u, texRes.width >> mipLevel);
    Uint32 mipHeight = std::max(1u, texRes.height >> mipLevel);
    Uint32 bytesPerRow = mipWidth * texRes.bytesPerPixel;
    Uint32 bytesPerImage = bytesPerRow * mipHeight;

    if (texture->storageMode() == MTL::StorageModePrivate) {
        // GPU-only texture: copy through the staging ring into the batched
        // upload stream
        size_t srcOffset;
        MTL::Buffer* srcBuf = stageData(data, size, srcOffset);
        ensureUploadBlit()->copyFromBuffer(
            srcBuf, srcOffset, bytesPerRow, bytesPerImage,
            MTL::Size::Make(mipWidth, mipHeight, 1),
            texture, arrayLayer, mipLevel, MTL::Origin::Make(0, 0, 0));
    } else {
        // CPU-accessible texture: direct write
        MTL::Region region(0, 0, 0, mipWidth, mipHeight, 1);
        texture->replaceRegion(region, mipLevel, arrayLayer, data, bytesPerRow, 0);
    }
}

void RHI_Metal::generateMipmaps(TextureHandle handle) {
    auto it = textures.find(handle.id);
    if (it == textures.end()) {
        return;
    }
    ensureUploadBlit()->generateMipmaps(it->second.texture.get());
}

void RHI_Metal::copyTexture(TextureHandle src, Uint32 srcMip, TextureHandle dst, Uint32 dstMip) {
    if (!currentCommandBuffer) return;
    auto sit = textures.find(src.id);
    auto dit = textures.find(dst.id);
    if (sit == textures.end() || dit == textures.end() || src.id == dst.id) return;

    // Must record on the frame command buffer (not the upload blit, which runs
    // before the frame) so the copy is ordered between the Hi-Z reduce passes.
    // One encoder at a time: close any open render/compute encoder first.
    if (currentRenderEncoder) { currentRenderEncoder->endEncoding(); currentRenderEncoder = nullptr; }
    if (currentComputeEncoder) { currentComputeEncoder->endEncoding(); currentComputeEncoder = nullptr; }

    MTL::Texture* s = sit->second.texture.get();
    MTL::Texture* d = dit->second.texture.get();
    MTL::Size size = MTL::Size::Make(std::max<NS::UInteger>(1, d->width() >> dstMip),
                                     std::max<NS::UInteger>(1, d->height() >> dstMip), 1);
    auto blit = currentCommandBuffer->blitCommandEncoder();
    blit->copyFromTexture(s, 0, srcMip, MTL::Origin::Make(0, 0, 0), size,
                          d, 0, dstMip, MTL::Origin::Make(0, 0, 0));
    blit->endEncoding();
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
    // Throttle the CPU to getMaxFramesInFlight() frames ahead of the GPU. The
    // permit is returned by this frame's completion handler (endFrame) or, if
    // this frame is skipped for want of a drawable, immediately below.
    if (frameSemaphore) {
        dispatch_semaphore_wait(frameSemaphore, DISPATCH_TIME_FOREVER);
        frameSemaphoreAcquired = true;
    }

    // Open this frame's autorelease pool before anything that can produce an
    // autoreleased object (uploads create command buffers). Drained in
    // endFrame; see the member declaration for why this is load-bearing.
    framePool = NS::AutoreleasePool::alloc()->init();

    // Commit pending uploads first: queue submission order makes the data
    // visible to this frame's commands without a CPU wait. Completed upload
    // command buffers are dropped opportunistically to bound the list.
    submitUploads(false);
    for (size_t i = 0; i < pendingUploadCmds.size();) {
        if (pendingUploadCmds[i]->status() == MTL::CommandBufferStatusCompleted) {
            pendingUploadCmds.erase(pendingUploadCmds.begin() + i);
        } else {
            ++i;
        }
    }
    if (pendingUploadCmds.empty() && !uploadCmdBuffer) {
        stagingRingOffset = 0;
        oversizeStaging.clear();
    } else if (stagingRingOffset > STAGING_RING_SIZE / 2) {
        // Ratchet guard (same bug as the Vulkan backend): with uploads
        // submitted every frame, one still-running upload command buffer here
        // keeps the opportunistic rewind above from ever firing; the offset
        // climbs across frames until the ring is full and every allocStaging()
        // wraps into submitUploads(true) — a mid-frame waitUntilCompleted per
        // updateBuffer call, decaying the frame rate over minutes. Drain here
        // instead: these command buffers are from previous frames, so the wait
        // is near-zero, and the ring provably rewinds once per frame.
        submitUploads(true);
    }

    // Leak-hunt telemetry moved to the StatsLog "MTL" source (registered in
    // initialize()). Whatever climbs across lines is the leak; flat lines mean
    // the decay is a stall, not memory.

    // Get next drawable. This legitimately fails when the window is occluded
    // or minimized — skip the frame instead of crashing; endFrame() and every
    // command-recording call no-op while currentCommandBuffer is null.
    currentDrawable = swapchain->nextDrawable();
    if (!currentDrawable) {
        currentCommandBuffer = nullptr;
        // No command buffer will be committed this frame, so nothing would ever
        // signal the permit we took above — return it now to stay balanced.
        // (endFrame's skipped-frame path drains framePool.)
        if (frameSemaphore && frameSemaphoreAcquired) {
            dispatch_semaphore_signal(frameSemaphore);
            frameSemaphoreAcquired = false;
        }
        return;
    }

    // Create command buffer. VAPOR_METAL_DEBUG=1 requests per-encoder
    // execution status so a GPU fault names the pass that caused it (the
    // completed handler below prints Completed/Affected/Pending per encoder;
    // "Affected" is the faulting one).
    static const bool debugCB = std::getenv("VAPOR_METAL_DEBUG") != nullptr;
    if (debugCB) {
        auto cbDesc = NS::TransferPtr(MTL::CommandBufferDescriptor::alloc()->init());
        cbDesc->setErrorOptions(MTL::CommandBufferErrorOptionEncoderExecutionStatus);
        currentCommandBuffer = NS::RetainPtr(commandQueue->commandBuffer(cbDesc.get()));
    } else {
        currentCommandBuffer = NS::RetainPtr(commandQueue->commandBuffer());
    }
    if (!currentCommandBuffer) {
        throw std::runtime_error("Failed to create command buffer");
    }
    if (debugCB) {
        currentCommandBuffer->addCompletedHandler([](MTL::CommandBuffer* cb) {
            if (cb->status() != MTL::CommandBufferStatusError) return;
            NS::Error* err = cb->error();
            fprintf(stderr, "[RHI_Metal] COMMAND BUFFER FAULT: %s\n",
                    err && err->localizedDescription() ? err->localizedDescription()->utf8String()
                                                       : "(no description)");
            if (err && err->userInfo()) {
                auto* infos = static_cast<NS::Array*>(
                    err->userInfo()->object(MTL::CommandBufferEncoderInfoErrorKey));
                if (infos) {
                    for (NS::UInteger i = 0; i < infos->count(); i++) {
                        auto* info = static_cast<MTL::CommandBufferEncoderInfo*>(infos->object(i));
                        const char* state = "?";
                        switch (info->errorState()) {
                            case MTL::CommandEncoderErrorStateCompleted: state = "Completed"; break;
                            case MTL::CommandEncoderErrorStateAffected:  state = "AFFECTED";  break;
                            case MTL::CommandEncoderErrorStatePending:   state = "Pending";   break;
                            case MTL::CommandEncoderErrorStateFaulted:   state = "FAULTED";   break;
                            default: break;
                        }
                        fprintf(stderr, "  encoder '%s': %s\n",
                                info->label() ? info->label()->utf8String() : "(unnamed)", state);
                    }
                }
            }
            fflush(stderr);
        });
    }

    // Latch GPU timing for this frame (the flag may be toggled mid-frame from
    // the UI; slot bookkeeping must stay consistent within a frame).
    gpuTimingActiveThisFrame = gpuTimingEnabled && gpuTimingSupported;
    ++timingFrameCounter;
    latestTimingFrame.store(timingFrameCounter, std::memory_order_relaxed);
    if (gpuTimingActiveThisFrame) {
        // Rotate to this frame's timing region — but ONLY if its previous
        // completion handler has finished reading it. A handler starved past
        // kTimingRegions frames would otherwise race the GPU rewriting the
        // region (fresh begin paired with a frames-old end -> fabricated
        // multi-frame pass times). Skipping one frame of timing is always
        // preferable to publishing garbage; the skip is counted for the
        // [MTL] stats line.
        if (timingRegionBusy[timingRegion].load(std::memory_order_acquire)) {
            gpuTimingActiveThisFrame = false;
            statsTimingSkips.fetch_add(1, std::memory_order_relaxed);
        } else {
            timingBaseSlot = timingRegion * GPU_TIMER_SAMPLE_COUNT;
            nextTimingSlot = timingBaseSlot;
            timingRegion = (timingRegion + 1) % kTimingRegions;
        }
    }
    framePassSamples.clear();
}

void RHI_Metal::endFrame() {
    // Frame was skipped (no drawable available in beginFrame)
    if (!currentCommandBuffer) {
        currentDrawable = nullptr;
        if (framePool) { framePool->release(); framePool = nullptr; }
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

    // Return this frame's throttle permit once the GPU finishes with it. Capture
    // the semaphore by value (not `this`): it outlives every in-flight frame
    // (released only in shutdown, after waitIdle), so the handler is always safe.
    if (frameSemaphore && frameSemaphoreAcquired) {
        dispatch_semaphore_t sem = frameSemaphore;
        currentCommandBuffer->addCompletedHandler([sem](MTL::CommandBuffer*) {
            dispatch_semaphore_signal(sem);
        });
        frameSemaphoreAcquired = false;
    }

    // Present drawable
    if (currentDrawable) {
        currentCommandBuffer->presentDrawable(currentDrawable);
    }

    // Commit any pending mid-frame uploads (e.g. RmlUI glyph textures created
    // during UI rendering) ahead of the frame's command buffer — same-queue
    // commit order guarantees the blits complete before the frame samples
    // those textures. Otherwise they'd only run at the next beginFrame.
    submitUploads(false);

    // Commit command buffer
    currentCommandBuffer->commit();

    {
        static const char* dbgEnv = std::getenv("VAPOR_METAL_DEBUG");
        if (dbgEnv && dbgEnv[0] == '2') {
            static uint64_t dbgFrame = 0;
            fprintf(stderr, "[frame %llu committed]\n", (unsigned long long)dbgFrame++);
            fflush(stderr);
        }
    }

    // Reset frame state
    currentDrawable = nullptr;
    currentCommandBuffer = nullptr;

    // Drain this frame's autoreleased objects (encoders, pass-label strings,
    // drawable ref, Metal-internal temporaries). commit()/presentDrawable and
    // the TransferPtr'd command buffer hold their own refs on anything the GPU
    // still needs, so this only reclaims the transient frame garbage.
    if (framePool) { framePool->release(); framePool = nullptr; }
}

void RHI_Metal::beginRenderPass(const RenderPassDesc& desc) {
    if (!currentCommandBuffer) {
        return;  // frame was skipped
    }

    // Hang localization (VAPOR_METAL_DEBUG=2): on a hard GPU hang nothing
    // completes and no fault handler fires — but a terminal (or SSH session)
    // keeps the last line printed before the machine froze: that names the
    // pass being encoded when it happened.
    static const char* dbgEnv = std::getenv("VAPOR_METAL_DEBUG");
    if (dbgEnv && dbgEnv[0] == '2') {
        fprintf(stderr, "[pass] %s\n", desc.name ? desc.name : "(unnamed)");
        fflush(stderr);
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
            // Attachment 0 may target one cube face / array layer and mip
            // (IBL capture, prefilter chains).
            if (i == 0 && desc.colorArrayLayer != ~0u) {
                colorAttachment->setSlice(desc.colorArrayLayer);
            }
            if (i == 0 && desc.colorMipLevel != 0) {
                colorAttachment->setLevel(desc.colorMipLevel);
            }
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
        // Render into a single array slice (cascaded shadow maps, cube faces).
        if (desc.depthArrayLayer != ~0u) {
            depthAttachment->setSlice(desc.depthArrayLayer);
        }

        if (desc.loadDepth) {
            depthAttachment->setLoadAction(MTL::LoadActionLoad);
        } else {
            depthAttachment->setLoadAction(MTL::LoadActionClear);
            depthAttachment->setClearDepth(desc.clearDepth);
        }
        depthAttachment->setStoreAction(MTL::StoreActionStore);
    }

    // Attach GPU timestamp sampling for this pass (no-op when timing is off).
    // Window scheme: [StartOfFragment, EndOfFragment] — the pass's own fragment
    // cost. StartOfVertex is deliberately NOT used (it degenerates on TBDR; see
    // PassSampleInfo). Fragment windows serialize per pass, so their union is a
    // real occupancy figure.
    NS::UInteger timingBegin, timingEnd;
    if (allocateTimingSlots(desc.name, timingBegin, timingEnd)) {
        auto* sampleAttachment = renderPassDesc->sampleBufferAttachments()->object(0);
        sampleAttachment->setSampleBuffer(gpuTimerSampleBuffer.get());
        sampleAttachment->setStartOfFragmentSampleIndex(timingBegin);
        sampleAttachment->setEndOfFragmentSampleIndex(timingEnd);
    }

    // Create render command encoder
    currentRenderEncoder = currentCommandBuffer->renderCommandEncoder(renderPassDesc);
    renderPassDesc->release();

    if (!currentRenderEncoder) {
        throw std::runtime_error("Failed to create render command encoder");
    }

    // Pass name shows up in Xcode captures / Instruments
    if (desc.name) {
        currentRenderEncoder->setLabel(NS::String::string(desc.name, NS::UTF8StringEncoding));
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
            // Rendering into a mip level: the pass extent is the MIP's size,
            // not the base size (IBL prefilter mips asserted on scissor).
            if (desc.colorMipLevel > 0) {
                passWidth = std::max(1u, passWidth >> desc.colorMipLevel);
                passHeight = std::max(1u, passHeight >> desc.colorMipLevel);
            }
        }
    } else if (desc.colorAttachments.empty() &&
               desc.depthAttachment.isValid() && desc.depthAttachment.id != 0) {
        // Depth-only pass (shadow cascades): size from the depth attachment,
        // not the swapchain — the fallback mis-placed the viewport.
        auto it = textures.find(desc.depthAttachment.id);
        if (it != textures.end()) {
            passWidth = it->second.width;
            passHeight = it->second.height;
        }
    }

    // Remember the pass extent so setScissor can clamp to it — callers pass
    // scissors in their own coordinate space (e.g. RmlUI's framebuffer size),
    // which need not match the attachment (mismatched under VAPOR_RENDER_SCALE,
    // where the drawable is smaller than the UI's pixel size).
    currentPassWidth = passWidth;
    currentPassHeight = passHeight;

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

void RHI_Metal::setComputeBytes(const void* data, size_t size, Uint32 binding) {
    if (currentComputeEncoder && data && size > 0) {
        currentComputeEncoder->setBytes(data, size, binding);
    }
}

void RHI_Metal::setScissor(int32_t x, int32_t y, Uint32 width, Uint32 height) {
    if (!currentRenderEncoder) return;
    // Clamp to non-negative offsets; callers clamp extents to the pass size
    // (Metal validation rejects scissors beyond the attachment bounds).
    if (x < 0) { width += x; x = 0; }
    if (y < 0) { height += y; y = 0; }
    // Clamp to the current pass extent (Metal asserts on scissors past the
    // attachment bounds).
    if (currentPassWidth > 0 && Uint32(x) < currentPassWidth) {
        width = std::min(width, currentPassWidth - Uint32(x));
    } else if (currentPassWidth > 0) { width = 0; }
    if (currentPassHeight > 0 && Uint32(y) < currentPassHeight) {
        height = std::min(height, currentPassHeight - Uint32(y));
    } else if (currentPassHeight > 0) { height = 0; }
    if (width == 0 || height == 0) { width = std::max(1u, width); height = std::max(1u, height); x = 0; y = 0; }
    MTL::ScissorRect rect;
    rect.x = static_cast<NS::UInteger>(x);
    rect.y = static_cast<NS::UInteger>(y);
    rect.width = width;
    rect.height = height;
    currentRenderEncoder->setScissorRect(rect);
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
        // Mesh pipelines have no vertex stage: route subsequent vertex-stage
        // binds to the object+mesh stages and remember the threadgroup sizes.
        currentPipelineIsMesh = res.isMesh;
        currentTaskThreads = res.taskThreads;
        currentMeshThreads = res.meshThreads;
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
        if (currentPipelineIsMesh) {
            // Mesh pipelines have no vertex stage; the same data feeds the
            // object (task) and mesh stages at the same index.
            currentRenderEncoder->setObjectBuffer(it->second.buffer.get(), offset, binding);
            currentRenderEncoder->setMeshBuffer(it->second.buffer.get(), offset, binding);
        } else {
            currentRenderEncoder->setVertexBuffer(it->second.buffer.get(), offset, binding);
        }
    }
}

void RHI_Metal::drawMeshTasks(Uint32 groupCountX, Uint32 groupCountY, Uint32 groupCountZ) {
    if (!currentRenderEncoder || !currentPipelineIsMesh || groupCountX == 0) return;
    currentRenderEncoder->drawMeshThreadgroups(
        MTL::Size::Make(groupCountX, groupCountY, groupCountZ),
        MTL::Size::Make(currentTaskThreads, 1, 1),
        MTL::Size::Make(currentMeshThreads, 1, 1));
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
        if (currentPipelineIsMesh) {
            currentRenderEncoder->setObjectBytes(data, size, binding);
            currentRenderEncoder->setMeshBytes(data, size, binding);
        } else {
            currentRenderEncoder->setVertexBytes(data, size, binding);
        }
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

void RHI_Metal::drawIndexedIndirect(BufferHandle argsBuffer, size_t offset, Uint32 drawCount, Uint32 stride) {
    auto argsIt = buffers.find(argsBuffer.id);
    auto idxIt = buffers.find(currentIndexBuffer.id);
    if (argsIt == buffers.end() || idxIt == buffers.end() || !currentRenderEncoder) {
        return;
    }
    MTL::Buffer* args = argsIt->second.buffer.get();
    MTL::Buffer* indexBuffer = idxIt->second.buffer.get();
    // Metal draws one indirect command per call, so multi-draw expands to a loop.
    // Each command reads its own indexCount/firstIndex/baseVertex/baseInstance
    // from the args buffer; instanceCount == 0 makes the draw a GPU no-op. The
    // index-buffer offset is 0 because the command carries the absolute
    // firstIndex into the merged index buffer.
    for (Uint32 i = 0; i < drawCount; ++i) {
        currentRenderEncoder->drawIndexedPrimitives(
            currentPrimitiveType,
            MTL::IndexTypeUInt32,
            indexBuffer,
            static_cast<NS::UInteger>(0),
            args,
            static_cast<NS::UInteger>(offset + static_cast<size_t>(i) * stride)
        );
    }
}

// ============================================================================
// Indirect Command Buffers + bindless texture tables (the ICB draw mode)
// ============================================================================

IndirectCommandBufferHandle RHI_Metal::createIndirectCommandBuffer(Uint32 maxCommands) {
    if (!capabilities.indirectCommandBuffers || maxCommands == 0) return {};

    auto desc = NS::TransferPtr(MTL::IndirectCommandBufferDescriptor::alloc()->init());
    desc->setCommandTypes(MTL::IndirectCommandTypeDrawIndexed);
    // Commands inherit the encoder's pipeline and buffer bindings; each command
    // carries only the indexed-draw arguments (its index-buffer region,
    // baseVertex, baseInstance). Textures/samplers are encoder state anyway.
    desc->setInheritPipelineState(true);
    desc->setInheritBuffers(true);
    desc->setMaxVertexBufferBindCount(0);
    desc->setMaxFragmentBufferBindCount(0);

    auto icb = NS::TransferPtr(device->newIndirectCommandBuffer(
        desc.get(), maxCommands, MTL::ResourceStorageModePrivate));
    if (!icb) {
        fmt::print("Failed to create indirect command buffer ({} commands)\n", maxCommands);
        return {};
    }

    Uint32 id = nextICBId++;
    ICBResource res;
    res.icb = icb;
    res.maxCommands = maxCommands;
    icbs[id] = res;
    return IndirectCommandBufferHandle{id};
}

void RHI_Metal::destroyIndirectCommandBuffer(IndirectCommandBufferHandle handle) {
    icbs.erase(handle.id);
}

void RHI_Metal::bindComputeICB(Uint32 binding, IndirectCommandBufferHandle handle) {
    auto it = icbs.find(handle.id);
    if (it == icbs.end() || !currentComputeEncoder) return;
    ICBResource& res = it->second;

    // The kernel sees the ICB through a tiny argument buffer (MSL:
    // `device ICBContainer& { command_buffer icb; }`). Encode it once with an
    // argument encoder from the currently bound kernel function.
    if (!res.containerArgBuffer) {
        auto pit = computePipelines.find(currentComputePipeline.id);
        if (pit == computePipelines.end() || !pit->second.function) return;
        auto encoder = NS::TransferPtr(pit->second.function->newArgumentEncoder(binding));
        if (!encoder) {
            fmt::print("bindComputeICB: no argument encoder for kernel buffer({})\n", binding);
            return;
        }
        res.containerArgBuffer = NS::TransferPtr(
            device->newBuffer(encoder->encodedLength(), MTL::ResourceStorageModeShared));
        encoder->setArgumentBuffer(res.containerArgBuffer.get(), 0);
        encoder->setIndirectCommandBuffer(res.icb.get(), 0);
    }

    currentComputeEncoder->setBuffer(res.containerArgBuffer.get(), 0, binding);
    // Argument-buffer indirection: the kernel's writes to the ICB must be
    // declared to the encoder explicitly.
    currentComputeEncoder->useResource(res.icb.get(), MTL::ResourceUsageWrite);
}

void RHI_Metal::executeICB(IndirectCommandBufferHandle handle, Uint32 commandCount) {
    auto it = icbs.find(handle.id);
    if (it == icbs.end() || !currentRenderEncoder || commandCount == 0) return;
    Uint32 count = std::min(commandCount, it->second.maxCommands);
    currentRenderEncoder->executeCommandsInBuffer(it->second.icb.get(),
                                                  NS::Range::Make(0, count));
}

BufferHandle RHI_Metal::createTextureArgumentTable(ShaderHandle fragmentShader, Uint32 bufferIndex,
                                                   Uint32 entryCount, Uint32 texturesPerEntry) {
    auto sit = shaders.find(fragmentShader.id);
    if (sit == shaders.end() || !sit->second.function || entryCount == 0) return {};

    auto encoder = NS::TransferPtr(sit->second.function->newArgumentEncoder(bufferIndex));
    if (!encoder) {
        fmt::print("createTextureArgumentTable: no argument encoder at buffer({})\n", bufferIndex);
        return {};
    }
    // One struct per entry; entries are tightly packed at the encoded stride.
    NS::UInteger stride = encoder->encodedLength();
    auto buffer = NS::TransferPtr(device->newBuffer(stride * entryCount,
                                                    MTL::ResourceStorageModeShared));
    if (!buffer) return {};

    // Register the MTL::Buffer under a normal buffer id so setFragmentBuffer
    // binds the table like any other buffer; the table map shares the id.
    Uint32 id = nextBufferId++;
    BufferResource bufRes;
    bufRes.buffer = buffer;
    bufRes.size = stride * entryCount;
    bufRes.isMapped = false;
    bufRes.mappedPointer = nullptr;
    buffers[id] = bufRes;

    ArgumentTableResource table;
    table.buffer = buffer;
    table.encoder = encoder;
    table.stride = stride;
    table.bufferIndex = bufferIndex;
    table.entryCount = entryCount;
    table.texturesPerEntry = texturesPerEntry;
    argumentTables[id] = std::move(table);
    return BufferHandle{id};
}

void RHI_Metal::writeTextureArgumentTable(BufferHandle tableHandle, Uint32 entry, Uint32 slot,
                                          TextureHandle texture) {
    auto tit = argumentTables.find(tableHandle.id);
    auto xit = textures.find(texture.id);
    if (tit == argumentTables.end() || xit == textures.end()) return;
    ArgumentTableResource& table = tit->second;
    if (entry >= table.entryCount || slot >= table.texturesPerEntry) return;

    table.encoder->setArgumentBuffer(table.buffer.get(), table.stride * entry);
    table.encoder->setTexture(xit->second.texture.get(), slot);
    table.written[entry * table.texturesPerEntry + slot] = xit->second.texture;
    table.residencyDirty = true;
}

void RHI_Metal::bindTextureArgumentTable(BufferHandle tableHandle) {
    auto tit = argumentTables.find(tableHandle.id);
    if (tit == argumentTables.end() || !currentRenderEncoder) return;
    ArgumentTableResource& table = tit->second;

    // Bind the argument buffer at its fragment slot, then declare residency
    // for every texture it references (argument-buffer indirection).
    currentRenderEncoder->setFragmentBuffer(table.buffer.get(), 0, table.bufferIndex);

    if (table.residencyDirty) {
        table.residentResources.clear();
        // Dedup: many materials share default/white textures.
        std::unordered_map<MTL::Texture*, bool> seen;
        for (auto& [key, tex] : table.written) {
            if (tex && !seen.count(tex.get())) {
                seen[tex.get()] = true;
                table.residentResources.push_back(tex.get());
            }
        }
        table.residencyDirty = false;
    }
    if (!table.residentResources.empty()) {
        currentRenderEncoder->useResources(table.residentResources.data(),
                                           table.residentResources.size(),
                                           MTL::ResourceUsageRead,
                                           MTL::RenderStageFragment);
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

void* RHI_Metal::getBackendTexture(TextureHandle handle) const {
    auto it = textures.find(handle.id);
    return it != textures.end() ? (void*)it->second.texture.get() : nullptr;
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
    ComputePipelineResource res;
    res.pipeline = pipeline;
    res.function = it->second.function;  // for bindComputeICB's argument encoder
    res.tgX = std::max(1u, desc.threadGroupSizeX);
    res.tgY = std::max(1u, desc.threadGroupSizeY);
    res.tgZ = std::max(1u, desc.threadGroupSizeZ);
    computePipelines[id] = res;

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
        // CRITICAL: vertex/index data uploaded via updateBuffer() sits in the
        // batched upload stream (uploadCmdBuffer), which is only committed at
        // submitUploads()/endFrame. The BLAS build below runs on its OWN
        // command buffer, committed immediately — without flushing first it
        // reads the buffers BEFORE the copies execute, building the BLAS over
        // zeroed vertex data. Every triangle degenerates at the origin, every
        // ray misses, and RT shadow/AO silently output their no-hit defaults
        // (uniform white / R=1) while TLAS/bind diagnostics all look valid.
        submitUploads(true);

        // Collect ALL of this BLAS's geometry descriptors, then build ONE
        // acceleration structure containing them. The old loop rebuilt a
        // single-geometry BLAS per iteration and reassigned resource.accelStruct
        // each time, so only the LAST geometry survived (latent today because
        // each BLAS holds one mesh, but a landmine for any multi-geometry mesh).
        // Accumulating also collapses N commit+waitUntilCompleted stalls into 1.
        std::vector<NS::SharedPtr<MTL::AccelerationStructureTriangleGeometryDescriptor>> geoms;
        geoms.reserve(resource.geometries.size());
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
            geoms.push_back(geomDesc);
        }
        if (geoms.empty()) return;

        std::vector<NS::Object*> geomPtrs;
        geomPtrs.reserve(geoms.size());
        for (auto& g : geoms) geomPtrs.push_back(g.get());

        auto accelDesc = NS::TransferPtr(MTL::PrimitiveAccelerationStructureDescriptor::alloc()->init());
        // Autoreleased array, used synchronously below (build waits before this
        // returns), so a raw pointer under the frame pool is correct here.
        NS::Array* geomArray = NS::Array::array(geomPtrs.data(), geomPtrs.size());
        accelDesc->setGeometryDescriptors(geomArray);

        auto accelSizes = device->accelerationStructureSizes(accelDesc.get());
        resource.scratchBuffer = NS::TransferPtr(device->newBuffer(accelSizes.buildScratchBufferSize, MTL::ResourceStorageModePrivate));
        resource.accelStruct = NS::TransferPtr(device->newAccelerationStructure(accelSizes.accelerationStructureSize));

        auto cmdBuffer = commandQueue->commandBuffer();
        auto encoder = cmdBuffer->accelerationStructureCommandEncoder();
        encoder->buildAccelerationStructure(resource.accelStruct.get(), accelDesc.get(), resource.scratchBuffer.get(), 0);
        encoder->endEncoding();
        cmdBuffer->commit();
        cmdBuffer->waitUntilCompleted();
    } else {
        // Build the TLAS from the instance list (rebuilt whenever
        // updateAccelerationStructure() supplies new instances).
        if (resource.instances.empty()) return;

        // Deduped BLAS array; each instance descriptor indexes into it.
        std::vector<NS::Object*> blasObjects;
        std::unordered_map<Uint32, Uint32> blasIndex;
        std::vector<MTL::AccelerationStructureInstanceDescriptor> descriptors;
        descriptors.reserve(resource.instances.size());
        for (const auto& inst : resource.instances) {
            auto blasIt = accelStructs.find(inst.blas.id);
            if (blasIt == accelStructs.end() || !blasIt->second.accelStruct) continue;
            auto [mapIt, inserted] = blasIndex.try_emplace(
                inst.blas.id, static_cast<Uint32>(blasObjects.size()));
            if (inserted) {
                blasObjects.push_back(static_cast<NS::Object*>(blasIt->second.accelStruct.get()));
            }
            // Zero-initialize: garbage options bits (NonOpaque, winding flags)
            // make rays miss whole instances — the native renderer learned this
            // the hard way (see its accel-instance fill).
            MTL::AccelerationStructureInstanceDescriptor d{};
            for (int c = 0; c < 4; ++c)
                for (int r = 0; r < 3; ++r)
                    d.transformationMatrix.columns[c][r] = inst.transform[c][r];
            d.accelerationStructureIndex = mapIt->second;
            d.mask = inst.mask;
            d.options = MTL::AccelerationStructureInstanceOptionOpaque;
            d.intersectionFunctionTableOffset = 0;
            descriptors.push_back(d);
        }
        if (descriptors.empty()) {
            if (std::getenv("VAPOR_RT_DEBUG"))
                fprintf(stderr, "[RT] TLAS build: 0 valid descriptors (BLAS not built?) "
                                "-> accelStruct stays null -> rays miss\n");
            return;
        }

        // Per-frame TLAS/scratch/instance-buffer rotation (native renderer's
        // TLASBuffers[frameInFlight] scheme): never rebuild into the structure
        // an in-flight frame's ray dispatches may still be traversing — that
        // in-place rewrite is a GPU-level race that hard-hangs Apple GPUs.
        const Uint32 slot = resource.nextSlot;
        resource.nextSlot = (resource.nextSlot + 1) % AccelStructResource::kTlasSlots;

        size_t bytes = descriptors.size() * sizeof(MTL::AccelerationStructureInstanceDescriptor);
        auto& instBuf = resource.instanceSlots[slot];
        if (!instBuf || instBuf->length() < bytes) {
            instBuf = NS::TransferPtr(device->newBuffer(bytes, MTL::ResourceStorageModeShared));
        }
        std::memcpy(instBuf->contents(), descriptors.data(), bytes);
        resource.instanceBuffer = instBuf;
        // NS::Array::array() is an autoreleased class factory (+0) → RetainPtr,
        // not TransferPtr; the pool would otherwise free this array while
        // resource.blasArray still references it.
        resource.blasArray = NS::RetainPtr(NS::Array::array(blasObjects.data(), blasObjects.size()));

        auto tlasDesc = NS::TransferPtr(MTL::InstanceAccelerationStructureDescriptor::alloc()->init());
        tlasDesc->setInstancedAccelerationStructures(resource.blasArray.get());
        tlasDesc->setInstanceCount(descriptors.size());
        tlasDesc->setInstanceDescriptorBuffer(instBuf.get());

        auto sizes = device->accelerationStructureSizes(tlasDesc.get());
        auto& scratch = resource.scratchSlots[slot];
        if (!scratch || scratch->length() < sizes.buildScratchBufferSize) {
            scratch = NS::TransferPtr(
                device->newBuffer(sizes.buildScratchBufferSize, MTL::ResourceStorageModePrivate));
        }
        auto& tlas = resource.accelSlots[slot];
        if (!tlas || tlas->size() < sizes.accelerationStructureSize) {
            tlas = NS::TransferPtr(
                device->newAccelerationStructure(sizes.accelerationStructureSize));
        }
        // Consumers (setAccelerationStructure) always bind the slot built for
        // this frame.
        resource.accelStruct = tlas;
        resource.scratchBuffer = scratch;

        // Prefer the frame's command buffer so the build is ordered before this
        // frame's RT dispatches; fall back to a synchronous one-off build.
        if (currentCommandBuffer) {
            auto encoder = currentCommandBuffer->accelerationStructureCommandEncoder();
            encoder->buildAccelerationStructure(tlas.get(), tlasDesc.get(), scratch.get(), 0);
            encoder->endEncoding();
        } else {
            auto cmd = commandQueue->commandBuffer();
            auto encoder = cmd->accelerationStructureCommandEncoder();
            encoder->buildAccelerationStructure(tlas.get(), tlasDesc.get(), scratch.get(), 0);
            encoder->endEncoding();
            cmd->commit();
            cmd->waitUntilCompleted();
        }
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

void RHI_Metal::beginComputePass(const char* name) {
    // End any active render encoder
    if (currentRenderEncoder) {
        currentRenderEncoder->endEncoding();
        currentRenderEncoder = nullptr;
    }
    if (!name) name = "Compute";

    static const char* dbgEnv = std::getenv("VAPOR_METAL_DEBUG");
    if (dbgEnv && dbgEnv[0] == '2') {
        fprintf(stderr, "[pass] %s\n", name);
        fflush(stderr);
    }

    // Create compute encoder
    if (currentCommandBuffer && !currentComputeEncoder) {
        NS::UInteger timingBegin, timingEnd;
        if (allocateTimingSlots(name, timingBegin, timingEnd)) {
            // computePassDescriptor() is an autoreleased class factory (+0):
            // RetainPtr so the scope-end release and the frame pool's drain
            // don't double-free it.
            auto passDesc = NS::RetainPtr(MTL::ComputePassDescriptor::computePassDescriptor());
            auto* sampleAttachment = passDesc->sampleBufferAttachments()->object(0);
            sampleAttachment->setSampleBuffer(gpuTimerSampleBuffer.get());
            sampleAttachment->setStartOfEncoderSampleIndex(timingBegin);
            sampleAttachment->setEndOfEncoderSampleIndex(timingEnd);
            currentComputeEncoder = currentCommandBuffer->computeCommandEncoder(passDesc.get());
        } else {
            currentComputeEncoder = currentCommandBuffer->computeCommandEncoder();
        }
        if (currentComputeEncoder) {
            currentComputeEncoder->setLabel(NS::String::string(name, NS::UTF8StringEncoding));
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

void RHI_Metal::setComputeSampledTexture(Uint32 binding, TextureHandle texture, SamplerHandle sampler) {
    // Metal samples in compute the same way as in fragment: the texture at a
    // texture index plus a sampler state at the matching sampler index. (Unlike
    // Vulkan, there is no separate storage-vs-sampled layout to manage.)
    auto texIt = textures.find(texture.id);
    auto samplerIt = samplers.find(sampler.id);
    if (!currentComputeEncoder) return;
    if (texIt != textures.end()) {
        currentComputeEncoder->setTexture(texIt->second.texture.get(), binding);
    }
    if (samplerIt != samplers.end()) {
        currentComputeEncoder->setSamplerState(samplerIt->second.sampler.get(), binding);
    }
}

void RHI_Metal::setAccelerationStructure(Uint32 binding, AccelStructHandle accelStruct) {
    auto it = accelStructs.find(accelStruct.id);
    if (std::getenv("VAPOR_RT_DEBUG")) {
        bool found = it != accelStructs.end();
        fprintf(stderr, "[RT] setAccelerationStructure(bind=%u): found=%d encoder=%d accelStruct=%d%s\n",
                binding, found ? 1 : 0, currentComputeEncoder ? 1 : 0,
                (found && it->second.accelStruct) ? 1 : 0,
                (found && currentComputeEncoder && it->second.accelStruct) ? "" : "  <-- BIND SKIPPED (kernel sees null TLAS)");
    }
    if (it != accelStructs.end() && currentComputeEncoder && it->second.accelStruct) {
        currentComputeEncoder->setAccelerationStructure(it->second.accelStruct.get(), binding);
        // Residency: binding a TLAS does NOT make its indirectly-referenced
        // BLASes (or the instance buffer) resident. Rays hitting non-resident
        // structures fault the GPU — the command buffer never completes, the
        // drawable pool drains and nextDrawable() blocks forever.
        if (it->second.type == AccelStructType::TopLevel) {
            if (it->second.blasArray) {
                NS::Array* arr = it->second.blasArray.get();
                for (NS::UInteger i = 0; i < arr->count(); i++) {
                    currentComputeEncoder->useResource(
                        static_cast<MTL::Resource*>(arr->object(i)), MTL::ResourceUsageRead);
                }
            }
            if (it->second.instanceBuffer) {
                currentComputeEncoder->useResource(it->second.instanceBuffer.get(), MTL::ResourceUsageRead);
            }
        }
    }
}

void RHI_Metal::dispatch(Uint32 groupCountX, Uint32 groupCountY, Uint32 groupCountZ) {
    if (currentComputeEncoder) {
        // Threadgroup shape comes from the bound pipeline's desc (SPIR-V bakes
        // local_size into the shader; MSL supplies it at dispatch). The old
        // hardcoded 1x1x1 silently ran 1/64th of every 8x8 kernel.
        MTL::Size threadsPerGroup(1, 1, 1);
        auto it = computePipelines.find(currentComputePipeline.id);
        if (it != computePipelines.end()) {
            threadsPerGroup = MTL::Size(it->second.tgX, it->second.tgY, it->second.tgZ);
        }
        MTL::Size threadgroups(groupCountX, groupCountY, groupCountZ);
        currentComputeEncoder->dispatchThreadgroups(threadgroups, threadsPerGroup);
    }
}
