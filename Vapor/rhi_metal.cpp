#define NS_PRIVATE_IMPLEMENTATION
#define MTL_PRIVATE_IMPLEMENTATION
#define CA_PRIVATE_IMPLEMENTATION

#include "rhi_metal.hpp"
#include <fmt/core.h>
#include <stdexcept>
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

    // Get swapchain dimensions
    int width, height;
    SDL_GetWindowSize(window, &width, &height);
    swapchainWidth = static_cast<Uint32>(width);
    swapchainHeight = static_cast<Uint32>(height);

    return true;
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
    textureDesc->setTextureType(MTL::TextureType2D);
    textureDesc->setWidth(desc.width);
    textureDesc->setHeight(desc.height);
    textureDesc->setDepth(desc.depth);
    textureDesc->setMipmapLevelCount(desc.mipLevels);
    textureDesc->setArrayLength(desc.arrayLayers);
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

        // Get main function
        function = NS::TransferPtr(library->newFunction(NS::String::string("main0", NS::UTF8StringEncoding)));
        if (!function) {
            throw std::runtime_error("Failed to find shader entry point 'main0'");
        }
    } else if (desc.filepath) {
        // Load library from file
        NS::Error* error = nullptr;
        auto path = NS::String::string(desc.filepath, NS::UTF8StringEncoding);
        library = NS::TransferPtr(device->newLibrary(path, &error));

        if (!library || error) {
            throw std::runtime_error("Failed to load shader from file");
        }

        function = NS::TransferPtr(library->newFunction(NS::String::string("main0", NS::UTF8StringEncoding)));
        if (!function) {
            throw std::runtime_error("Failed to find shader entry point");
        }
    }

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
    samplerDesc->setMaxAnisotropy(desc.anisotropyEnable ? desc.maxAnisotropy : 1);
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
            colorAttachment->setAlphaBlendOperation(MTL::BlendOperationAdd);
            break;
        case BlendMode::Additive:
            colorAttachment->setBlendingEnabled(true);
            colorAttachment->setSourceRGBBlendFactor(MTL::BlendFactorOne);
            colorAttachment->setDestinationRGBBlendFactor(MTL::BlendFactorOne);
            colorAttachment->setRgbBlendOperation(MTL::BlendOperationAdd);
            colorAttachment->setSourceAlphaBlendFactor(MTL::BlendFactorOne);
            colorAttachment->setDestinationAlphaBlendFactor(MTL::BlendFactorZero);
            colorAttachment->setAlphaBlendOperation(MTL::BlendOperationAdd);
            break;
    }

    // Depth attachment
    if (desc.depthTest) {
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

    Uint32 id = nextPipelineId++;
    pipelines[id] = {pipeline, nullptr, false};

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

    void* bufferData = it->second.buffer->contents();
    std::memcpy(static_cast<char*>(bufferData) + offset, data, size);

    // Notify Metal of modified range if managed storage
    if (it->second.buffer->storageMode() == MTL::StorageModeManaged) {
        it->second.buffer->didModifyRange(NS::Range::Make(offset, size));
    }
}

void RHI_Metal::updateTexture(TextureHandle handle, const void* data, size_t size) {
    auto it = textures.find(handle.id);
    if (it == textures.end()) {
        return;
    }

    const TextureResource& texRes = it->second;

    // Calculate bytes per row
    Uint32 bytesPerPixel = 4; // Assume RGBA8 for now
    Uint32 bytesPerRow = texRes.width * bytesPerPixel;

    // Replace texture region
    MTL::Region region(0, 0, 0, texRes.width, texRes.height, 1);
    texRes.texture->replaceRegion(region, 0, data, bytesPerRow);
}

// ============================================================================
// Frame Operations
// ============================================================================

void RHI_Metal::beginFrame() {
    // Get next drawable
    currentDrawable = swapchain->nextDrawable();
    if (!currentDrawable) {
        throw std::runtime_error("Failed to get next drawable");
    }

    // Create command buffer
    currentCommandBuffer = NS::TransferPtr(commandQueue->commandBuffer());
    if (!currentCommandBuffer) {
        throw std::runtime_error("Failed to create command buffer");
    }
}

void RHI_Metal::endFrame() {
    // End any active encoder
    if (currentRenderEncoder) {
        currentRenderEncoder->endEncoding();
        currentRenderEncoder = nullptr;
    }
    if (currentComputeEncoder) {
        currentComputeEncoder->endEncoding();
        currentComputeEncoder = nullptr;
    }

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
            if (it != textures.end()) {
                colorAttachment->setTexture(it->second.texture.get());
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
        colorAttachment->setStoreAction(MTL::StoreActionStore);
    }

    // Setup depth attachment
    if (desc.depthAttachment.id != 0) {
        auto depthAttachment = renderPassDesc->depthAttachment();
        auto it = textures.find(desc.depthAttachment.id);
        if (it != textures.end()) {
            depthAttachment->setTexture(it->second.texture.get());
        }

        if (desc.loadDepth) {
            depthAttachment->setLoadAction(MTL::LoadActionLoad);
        } else {
            depthAttachment->setLoadAction(MTL::LoadActionClear);
            depthAttachment->setClearDepth(desc.clearDepth);
        }
        depthAttachment->setStoreAction(MTL::StoreActionStore);
    }

    // Create render command encoder
    currentRenderEncoder = currentCommandBuffer->renderCommandEncoder(renderPassDesc);
    renderPassDesc->release();

    if (!currentRenderEncoder) {
        throw std::runtime_error("Failed to create render command encoder");
    }
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
        currentRenderEncoder->setRenderPipelineState(it->second.renderPipeline.get());

        // Set default cull mode and winding
        currentRenderEncoder->setCullMode(MTL::CullModeBack);
        currentRenderEncoder->setFrontFacingWinding(MTL::WindingCounterClockwise);
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

void RHI_Metal::draw(Uint32 vertexCount, Uint32 instanceCount, Uint32 firstVertex, Uint32 firstInstance) {
    if (currentRenderEncoder) {
        currentRenderEncoder->drawPrimitives(
            MTL::PrimitiveTypeTriangle,
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
            MTL::PrimitiveTypeTriangle,
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
    return PixelFormat::BGRA8_UNORM_SRGB;
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

// ============================================================================
// Format Conversion Helpers
// ============================================================================

MTL::PixelFormat RHI_Metal::convertPixelFormat(PixelFormat format) {
    switch (format) {
        case PixelFormat::RGBA8_UNORM: return MTL::PixelFormatRGBA8Unorm;
        case PixelFormat::RGBA8_SRGB: return MTL::PixelFormatRGBA8Unorm_sRGB;
        case PixelFormat::BGRA8_UNORM: return MTL::PixelFormatBGRA8Unorm;
        case PixelFormat::BGRA8_UNORM_SRGB: return MTL::PixelFormatBGRA8Unorm_sRGB;
        case PixelFormat::RGBA16_FLOAT: return MTL::PixelFormatRGBA16Float;
        case PixelFormat::RGBA32_FLOAT: return MTL::PixelFormatRGBA32Float;
        case PixelFormat::R8_UNORM: return MTL::PixelFormatR8Unorm;
        case PixelFormat::R16_FLOAT: return MTL::PixelFormatR16Float;
        case PixelFormat::R32_FLOAT: return MTL::PixelFormatR32Float;
        case PixelFormat::D32_FLOAT: return MTL::PixelFormatDepth32Float;
        case PixelFormat::D24_UNORM_S8_UINT: return MTL::PixelFormatDepth24Unorm_Stencil8;
        default: return MTL::PixelFormatRGBA8Unorm;
    }
}

MTL::TextureUsage RHI_Metal::convertTextureUsage(Uint32 usage) {
    MTL::TextureUsage metalUsage = MTL::TextureUsageUnknown;

    if (usage & (Uint32)TextureUsage::Sampled) {
        metalUsage |= MTL::TextureUsageShaderRead;
    }
    if (usage & (Uint32)TextureUsage::Storage) {
        metalUsage |= MTL::TextureUsageShaderWrite;
    }
    if (usage & (Uint32)TextureUsage::ColorAttachment) {
        metalUsage |= MTL::TextureUsageRenderTarget;
    }
    if (usage & (Uint32)TextureUsage::DepthStencilAttachment) {
        metalUsage |= MTL::TextureUsageRenderTarget;
    }

    return metalUsage;
}

MTL::SamplerAddressMode RHI_Metal::convertSamplerAddressMode(SamplerAddressMode mode) {
    switch (mode) {
        case SamplerAddressMode::Repeat: return MTL::SamplerAddressModeRepeat;
        case SamplerAddressMode::MirroredRepeat: return MTL::SamplerAddressModeMirrorRepeat;
        case SamplerAddressMode::ClampToEdge: return MTL::SamplerAddressModeClampToEdge;
        case SamplerAddressMode::ClampToBorder: return MTL::SamplerAddressModeClampToZero;
        default: return MTL::SamplerAddressModeRepeat;
    }
}

MTL::SamplerMinMagFilter RHI_Metal::convertSamplerFilter(SamplerFilter filter) {
    switch (filter) {
        case SamplerFilter::Nearest: return MTL::SamplerMinMagFilterNearest;
        case SamplerFilter::Linear: return MTL::SamplerMinMagFilterLinear;
        default: return MTL::SamplerMinMagFilterLinear;
    }
}

MTL::SamplerMipFilter RHI_Metal::convertSamplerMipFilter(SamplerFilter filter) {
    switch (filter) {
        case SamplerFilter::Nearest: return MTL::SamplerMipFilterNearest;
        case SamplerFilter::Linear: return MTL::SamplerMipFilterLinear;
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
            if (vertexBufIt == buffers.end() || indexBufIt == indexBufIt.end()) {
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
            auto geomArray = NS::Array::array(static_cast<NS::Object*>(geomDesc.get()), 1);
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
        currentComputeEncoder = currentCommandBuffer->computeCommandEncoder();
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
