#pragma once
#include <SDL3/SDL_stdinc.h>
#include <cstdint>
#include <string>
#include <vector>
#include <glm/vec4.hpp>  // Full definition needed for RenderPassDesc::clearColors (std::vector<glm::vec4>)
#include <glm/mat4x4.hpp>  // Full definition needed for AccelStructInstance::transform

// ============================================================================
// RHI (Render Hardware Interface) Layer
//
// This layer provides a thin abstraction over GPU APIs (Vulkan, Metal, etc.)
// It manages GPU resources and command recording, but does NOT contain
// high-level rendering logic.
// ============================================================================

// Forward declarations
struct SDL_Window;

// ============================================================================
// Handle Types
//
// Conventions:
//  - A default-constructed handle (id == UINT32_MAX) is INVALID ("none").
//  - Backend resource ids start at 1; id 0 is reserved.
//  - For RenderPassDesc color attachments, TextureHandle{0} means "the current
//    swapchain drawable". An invalid depth attachment means "no depth".
//  - Ids are monotonically increasing and NEVER reused within a session, so a
//    stale handle (resource destroyed) can never alias a newer resource — it
//    simply misses the backend's lookup and the call becomes a no-op. This
//    gives the safety property of generational handles; explicit generation
//    bits only become necessary if storage moves to dense free-list arrays.
// ============================================================================

struct BufferHandle {
    Uint32 id = UINT32_MAX;
    bool isValid() const { return id != UINT32_MAX; }
};

struct TextureHandle {
    Uint32 id = UINT32_MAX;
    bool isValid() const { return id != UINT32_MAX; }
};

struct ShaderHandle {
    Uint32 id = UINT32_MAX;
    bool isValid() const { return id != UINT32_MAX; }
};

struct PipelineHandle {
    Uint32 id = UINT32_MAX;
    bool isValid() const { return id != UINT32_MAX; }
};

struct RenderPassHandle {
    Uint32 id = UINT32_MAX;
    bool isValid() const { return id != UINT32_MAX; }
};

struct ComputePipelineHandle {
    Uint32 id = UINT32_MAX;
    bool isValid() const { return id != UINT32_MAX; }
};

struct AccelStructHandle {
    Uint32 id = UINT32_MAX;
    bool isValid() const { return id != UINT32_MAX; }
};

struct SamplerHandle {
    Uint32 id = UINT32_MAX;
    bool isValid() const { return id != UINT32_MAX; }
};

struct DescriptorSetHandle {
    Uint32 id = UINT32_MAX;
    bool isValid() const { return id != UINT32_MAX; }
};

// ============================================================================
// Enums
// ============================================================================

enum class BufferUsage {
    Vertex,
    Index,
    Uniform,
    Storage,
    TransferSrc,
    TransferDst,
    // GPU-driven rendering: a buffer that holds indirect draw arguments produced
    // by a compute pass. On Vulkan this also carries STORAGE (so compute can
    // write it) + INDIRECT; on Metal buffers are usage-agnostic.
    Indirect,
};

enum class MemoryUsage {
    GPU,           // GPU-only memory (best performance)
    CPU,           // CPU-only memory (for readback)
    CPUtoGPU,      // CPU writes, GPU reads (for dynamic data)
    GPUreadback,   // GPU writes, CPU reads
};

enum class PixelFormat {
    RGBA8_UNORM,
    RGBA8_SRGB,
    RGBA16_FLOAT,
    RGBA32_FLOAT,
    BGRA8_UNORM,
    BGRA8_SRGB,
    R8_UNORM,
    R16_FLOAT,
    R32_FLOAT,
    // Vertex-attribute-only formats (vec2/vec3 inputs). Not valid as texture
    // formats on Metal (RGB32 is not renderable there).
    RG32_FLOAT,
    RGB32_FLOAT,
    Depth32Float,
    Depth24Stencil8,
    // Sentinel: "whatever the swapchain format is". Only valid in
    // PipelineDesc attachment formats; resolved by the backend.
    Swapchain,
};

// Bytes per pixel for CPU<->GPU copies (tightly packed).
inline Uint32 pixelFormatBytesPerPixel(PixelFormat format) {
    switch (format) {
        case PixelFormat::R8_UNORM: return 1;
        case PixelFormat::R16_FLOAT: return 2;
        case PixelFormat::R32_FLOAT: return 4;
        case PixelFormat::RG32_FLOAT: return 8;
        case PixelFormat::RGB32_FLOAT: return 12;
        case PixelFormat::RGBA16_FLOAT: return 8;
        case PixelFormat::RGBA32_FLOAT: return 16;
        case PixelFormat::Depth32Float: return 4;
        case PixelFormat::Depth24Stencil8: return 4;
        case PixelFormat::RGBA8_UNORM:
        case PixelFormat::RGBA8_SRGB:
        case PixelFormat::BGRA8_UNORM:
        case PixelFormat::BGRA8_SRGB:
        case PixelFormat::Swapchain:
        default: return 4;
    }
}

// Bitmask — combine with operator| (e.g. RenderTarget | Sampled for a render
// target that is later sampled by a post-process pass).
enum class TextureUsage : Uint32 {
    Sampled      = 1 << 0,
    Storage      = 1 << 1,
    RenderTarget = 1 << 2,
    DepthStencil = 1 << 3,
};

inline TextureUsage operator|(TextureUsage a, TextureUsage b) {
    return static_cast<TextureUsage>(static_cast<Uint32>(a) | static_cast<Uint32>(b));
}

inline bool hasUsage(TextureUsage value, TextureUsage flag) {
    return (static_cast<Uint32>(value) & static_cast<Uint32>(flag)) != 0;
}

enum class FilterMode {
    Nearest,
    Linear,
};

enum class AddressMode {
    Repeat,
    ClampToEdge,
    ClampToBorder,
    MirrorRepeat,
};

enum class CompareOp {
    Never,
    Less,
    Equal,
    LessOrEqual,
    Greater,
    NotEqual,
    GreaterOrEqual,
    Always,
};

enum class BlendMode {
    Opaque,
    AlphaBlend,
    Additive,
    Multiply,
};

enum class CullMode {
    None,
    Front,
    Back,
};

enum class PrimitiveTopology {
    PointList,
    LineList,
    LineStrip,
    TriangleList,
    TriangleStrip,
};

enum class ShaderStage {
    Vertex,
    Fragment,
    Compute,
};

// ============================================================================
// Descriptors
// ============================================================================

struct BufferDesc {
    size_t size = 0;
    BufferUsage usage = BufferUsage::Vertex;
    MemoryUsage memoryUsage = MemoryUsage::GPU;
};

struct TextureDesc {
    Uint32 width = 1;
    Uint32 height = 1;
    Uint32 depth = 1;
    Uint32 mipLevels = 1;
    Uint32 arrayLayers = 1;  // 6 for cubemaps; >1 (non-cube) = 2D array
    Uint32 sampleCount = 1;  // For MSAA render targets
    bool isCube = false;     // Requires arrayLayers == 6
    PixelFormat format = PixelFormat::RGBA8_UNORM;
    TextureUsage usage = TextureUsage::Sampled;
};

struct SamplerDesc {
    FilterMode minFilter = FilterMode::Linear;
    FilterMode magFilter = FilterMode::Linear;
    FilterMode mipFilter = FilterMode::Linear;
    AddressMode addressModeU = AddressMode::Repeat;
    AddressMode addressModeV = AddressMode::Repeat;
    AddressMode addressModeW = AddressMode::Repeat;
    float mipLodBias = 0.0f;
    float maxAnisotropy = 1.0f;
    bool enableAnisotropy = false;
    bool enableCompare = false;
    CompareOp compareOp = CompareOp::Less;
    float minLod = 0.0f;
    float maxLod = 1000.0f;
};

struct ShaderDesc {
    ShaderStage stage;
    const void* code;
    size_t codeSize;
    const char* entryPoint = "main";
};

struct VertexAttribute {
    Uint32 location;
    PixelFormat format;
    Uint32 offset;
};

struct VertexLayout {
    std::vector<VertexAttribute> attributes;
    Uint32 stride;
};

struct PipelineDesc {
    ShaderHandle vertexShader;
    ShaderHandle fragmentShader;
    VertexLayout vertexLayout;
    PrimitiveTopology topology = PrimitiveTopology::TriangleList;
    BlendMode blendMode = BlendMode::Opaque;
    // Depth state. depthWrite only takes effect while depthTest is enabled
    // (matching Vulkan semantics; Metal is made to match in the backend).
    bool depthTest = true;
    bool depthWrite = true;
    CompareOp depthCompareOp = CompareOp::Less;
    // Whether the render pass this pipeline draws into has a depth attachment.
    // Both Metal and Vulkan bake attachment formats into the pipeline object,
    // and a mismatch with the actual pass is a validation error. Set false for
    // passes without depth (e.g. fullscreen post-process to swapchain).
    bool hasDepthAttachment = true;
    CullMode cullMode = CullMode::Back;
    bool frontFaceCounterClockwise = true;
    Uint32 sampleCount = 1;
    // Attachment formats this pipeline renders into. Both Metal and Vulkan
    // bake these into the pipeline object; they must match the render pass.
    // PixelFormat::Swapchain resolves to the actual swapchain format.
    std::vector<PixelFormat> colorAttachmentFormats = { PixelFormat::Swapchain };
    PixelFormat depthAttachmentFormat = PixelFormat::Depth32Float;  // when hasDepthAttachment
    // TODO: Add descriptor set layouts when needed.
};

struct RenderPassDesc {
    // Debug/profiling label for this pass (shown in GPU timings and captures).
    // Must point to storage that outlives the beginRenderPass() call (a string
    // literal is the expected use).
    const char* name = nullptr;

    // Color attachments. TextureHandle{0} = current swapchain drawable.
    std::vector<TextureHandle> colorAttachments;
    // Optional per-color-attachment MSAA resolve targets. When set (valid) for
    // index i, colorAttachments[i] is expected to be multisampled and is
    // resolved into resolveAttachments[i] at the end of the pass.
    std::vector<TextureHandle> resolveAttachments;

    // Depth attachment. Invalid handle = no depth.
    TextureHandle depthAttachment;
    // When the depth attachment is an array texture (e.g. a cascaded shadow
    // map), render into this single array layer. UINT32_MAX = use the whole
    // texture's default view (the normal 2D case). Backends build a per-layer
    // view (Vulkan) / set the render-target slice (Metal).
    Uint32 depthArrayLayer = ~0u;
    // Same for color attachment 0: render into one cube face / array layer
    // and/or one mip level (IBL capture, prefilter chains). UINT32_MAX layer =
    // whole-texture view; mip defaults to 0.
    Uint32 colorArrayLayer = ~0u;
    Uint32 colorMipLevel = 0;

    // Clear values
    std::vector<glm::vec4> clearColors;
    float clearDepth = 1.0f;
    Uint32 clearStencil = 0;

    // Load/store operations (true = load, false = clear)
    std::vector<bool> loadColor;
    bool loadDepth = false;
};

struct ComputePipelineDesc {
    ShaderHandle computeShader;
    // Thread group sizes (for validation/documentation)
    Uint32 threadGroupSizeX = 1;
    Uint32 threadGroupSizeY = 1;
    Uint32 threadGroupSizeZ = 1;
};

enum class AccelStructType {
    BottomLevel,  // BLAS - geometry level
    TopLevel      // TLAS - instance level
};

struct AccelStructGeometry {
    BufferHandle vertexBuffer;
    Uint32 vertexCount;
    Uint32 vertexStride;
    BufferHandle indexBuffer;
    Uint32 indexCount;
    BufferHandle transformBuffer; // Optional transform matrix
};

struct AccelStructInstance {
    AccelStructHandle blas;
    glm::mat4 transform;
    Uint32 instanceID;
    Uint32 mask;
};

struct AccelStructDesc {
    AccelStructType type;

    // For BLAS
    std::vector<AccelStructGeometry> geometries;

    // For TLAS
    std::vector<AccelStructInstance> instances;

    bool allowUpdate = false;
    bool preferFastBuild = false;
};

// ============================================================================
// Capabilities
// ============================================================================

// Hardware/backend feature support, queried once at initialization.
// The renderer's RenderGraph uses this to skip passes whose declared
// requirements (PassFlags) the active backend cannot satisfy — this is how
// single-backend features (e.g. raytracing on Metal) are expressed without
// backend checks in rendering code.
struct RHICapabilities {
    bool raytracing = false;     // acceleration structures + ray queries
    bool computeShaders = false; // compute pipelines with resource binding
    bool gpuTimestamps = false;  // per-pass GPU timing (see GPU Profiling)
};

// ============================================================================
// GPU Profiling
// ============================================================================

struct GpuPassTiming {
    std::string name;
    double gpuTimeMs = 0.0;
};

// ============================================================================
// RHI Interface
// ============================================================================

class RHI {
public:
    virtual ~RHI() = default;

    // ========================================================================
    // Initialization
    // ========================================================================

    virtual bool initialize(SDL_Window* window) = 0;
    virtual void shutdown() = 0;
    virtual void waitIdle() = 0;

    // Feature support of this backend/device. Valid after initialize().
    virtual const RHICapabilities& getCapabilities() const = 0;

    // How many frames the CPU may record ahead of the GPU. Per-frame
    // resources (upload rings, per-frame buffers) must be sized by this.
    virtual Uint32 getMaxFramesInFlight() const = 0;

    // ========================================================================
    // Resource Creation
    // ========================================================================

    virtual BufferHandle createBuffer(const BufferDesc& desc) = 0;
    virtual void destroyBuffer(BufferHandle handle) = 0;

    virtual TextureHandle createTexture(const TextureDesc& desc) = 0;
    virtual void destroyTexture(TextureHandle handle) = 0;

    virtual ShaderHandle createShader(const ShaderDesc& desc) = 0;
    virtual void destroyShader(ShaderHandle handle) = 0;

    virtual SamplerHandle createSampler(const SamplerDesc& desc) = 0;
    virtual void destroySampler(SamplerHandle handle) = 0;

    virtual PipelineHandle createPipeline(const PipelineDesc& desc) = 0;
    virtual void destroyPipeline(PipelineHandle handle) = 0;

    virtual ComputePipelineHandle createComputePipeline(const ComputePipelineDesc& desc) = 0;
    virtual void destroyComputePipeline(ComputePipelineHandle handle) = 0;

    virtual AccelStructHandle createAccelerationStructure(const AccelStructDesc& desc) = 0;
    virtual void destroyAccelerationStructure(AccelStructHandle handle) = 0;
    virtual void buildAccelerationStructure(AccelStructHandle handle) = 0;
    virtual void updateAccelerationStructure(AccelStructHandle handle, const std::vector<AccelStructInstance>& instances) = 0;

    // ========================================================================
    // Resource Updates
    // ========================================================================

    virtual void updateBuffer(BufferHandle handle, const void* data, size_t offset, size_t size) = 0;

    // Upload tightly-packed pixel data to one mip level of one array layer.
    virtual void updateTexture(TextureHandle handle, const void* data, size_t size,
                               Uint32 mipLevel, Uint32 arrayLayer) = 0;
    // Convenience: level 0, layer 0.
    void updateTexture(TextureHandle handle, const void* data, size_t size) {
        updateTexture(handle, data, size, 0, 0);
    }

    // Generate the full mip chain from level 0 (all array layers).
    // Recorded into the batched upload stream like updateTexture.
    virtual void generateMipmaps(TextureHandle handle) = 0;

    // Uploads to GPU-only resources are recorded into a shared upload command
    // stream and submitted automatically before the next frame's rendering
    // (or when the staging ring fills). Call this to force an immediate
    // submit + wait — e.g. before reading back, or at the end of a loading
    // phase when you want the transfer cost accounted there.
    virtual void flushUploads() = 0;

    // Copy swapchain/texture to CPU-readable buffer for screenshot
    // Returns a buffer handle that can be mapped after the copy completes
    virtual BufferHandle copySwapchainToBuffer(Uint32& outWidth, Uint32& outHeight) = 0;

    // Map buffer memory for CPU read (returns nullptr on failure)
    virtual void* mapBuffer(BufferHandle handle) = 0;
    virtual void unmapBuffer(BufferHandle handle) = 0;

    // ========================================================================
    // Frame Operations
    // ========================================================================

    virtual void beginFrame() = 0;
    virtual void endFrame() = 0;

    virtual void beginRenderPass(const RenderPassDesc& desc) = 0;
    virtual void endRenderPass() = 0;

    // ========================================================================
    // Rendering Commands
    // ========================================================================

    virtual void bindPipeline(PipelineHandle pipeline) = 0;
    virtual void bindVertexBuffer(BufferHandle buffer, Uint32 binding = 0, size_t offset = 0) = 0;
    virtual void bindIndexBuffer(BufferHandle buffer, size_t offset = 0) = 0;

    // Descriptor binding (simplified for now)
    virtual void setUniformBuffer(Uint32 set, Uint32 binding, BufferHandle buffer, size_t offset = 0, size_t range = 0) = 0;
    virtual void setStorageBuffer(Uint32 set, Uint32 binding, BufferHandle buffer, size_t offset = 0, size_t range = 0) = 0;
    virtual void setTexture(Uint32 set, Uint32 binding, TextureHandle texture, SamplerHandle sampler) = 0;

    // Stage-specific buffer binding.
    // Required for backends (e.g. Metal) where vertex and fragment shaders have
    // independent buffer index namespaces. setUniformBuffer() binds to BOTH stages
    // at the same index, which collides when a binding means different things per
    // stage (e.g. vertex buffer(0)=camera vs fragment buffer(0)=lights).
    virtual void setVertexBuffer(Uint32 binding, BufferHandle buffer, size_t offset = 0, size_t range = 0) = 0;
    virtual void setFragmentBuffer(Uint32 binding, BufferHandle buffer, size_t offset = 0, size_t range = 0) = 0;

    // Direct data binding (for small constants like instanceID)
    virtual void setVertexBytes(const void* data, size_t size, Uint32 binding) = 0;
    virtual void setFragmentBytes(const void* data, size_t size, Uint32 binding) = 0;

    virtual void draw(Uint32 vertexCount, Uint32 instanceCount = 1, Uint32 firstVertex = 0, Uint32 firstInstance = 0) = 0;
    virtual void drawIndexed(Uint32 indexCount, Uint32 instanceCount = 1, Uint32 firstIndex = 0, int32_t vertexOffset = 0, Uint32 firstInstance = 0) = 0;

    // Multi-draw indirect: issue `drawCount` indexed draws whose arguments are
    // read from `argsBuffer` starting at `offset`, one DrawCommand every
    // `stride` bytes (see the DrawCommand struct in graphics_gpu_structs.hpp).
    // The currently bound index buffer + a merged vertex buffer are used; each
    // command carries its own firstIndex/vertexOffset/firstInstance. On Vulkan
    // this is a single vkCmdDrawIndexedIndirect; on Metal it expands to a loop
    // of per-command indirect draws (same observable result). A command with
    // instanceCount == 0 draws nothing, which is how the cull pass drops objects.
    virtual void drawIndexedIndirect(BufferHandle argsBuffer, size_t offset, Uint32 drawCount, Uint32 stride) = 0;

    // ========================================================================
    // Compute Commands
    // ========================================================================

    // `name` labels the pass in GPU timings / captures (default keeps the old
    // generic "Compute" — pass the real pass name so profilers can tell the
    // RT / AO / tile-cull dispatches apart).
    virtual void beginComputePass(const char* name = "Compute") = 0;
    virtual void endComputePass() = 0;
    virtual void bindComputePipeline(ComputePipelineHandle pipeline) = 0;
    virtual void setComputeBuffer(Uint32 binding, BufferHandle buffer, size_t offset = 0, size_t range = 0) = 0;
    virtual void setComputeTexture(Uint32 binding, TextureHandle texture) = 0;
    virtual void setAccelerationStructure(Uint32 binding, AccelStructHandle accelStruct) = 0;
    // Small inline constants for compute (Metal: setBytes at the given buffer
    // index; Vulkan: compute push constants at (binding%4)*16, 16 bytes/slot).
    virtual void setComputeBytes(const void* /*data*/, size_t /*size*/, Uint32 /*binding*/) {}
    virtual void dispatch(Uint32 groupCountX, Uint32 groupCountY = 1, Uint32 groupCountZ = 1) = 0;
    // Barrier so compute-shader writes to a buffer are visible to subsequent
    // compute/vertex/fragment reads (e.g. GPU particle sim -> instanced draw).
    // Default no-op: Metal's tracked hazard mode inserts these automatically.
    virtual void computeBarrier() {}

    // Set the scissor rectangle for subsequent draws, in framebuffer pixels
    // (top-left origin). Only valid inside a render pass; beginRenderPass
    // resets it to the full render area. Used by UI clipping (RmlUI).
    virtual void setScissor(int32_t /*x*/, int32_t /*y*/, Uint32 /*width*/, Uint32 /*height*/) {}

    // ========================================================================
    // Utility
    // ========================================================================

    virtual Uint32 getSwapchainWidth() const = 0;
    virtual Uint32 getSwapchainHeight() const = 0;
    virtual PixelFormat getSwapchainFormat() const = 0;

    // ========================================================================
    // GPU Profiling (optional; default no-op)
    // ========================================================================
    // Per-pass GPU timestamps. Passes are identified by RenderPassDesc::name.
    // Results lag the submitted frame by at least one frame (resolved on
    // command-buffer completion).

    virtual bool isGpuTimingSupported() const { return false; }
    virtual void setGpuTimingEnabled(bool /*enabled*/) {}
    virtual bool isGpuTimingEnabled() const { return false; }
    virtual std::vector<GpuPassTiming> getGpuPassTimings() { return {}; }

    // ========================================================================
    // Backend Query Interface (for backend-specific operations)
    // ========================================================================
    // These methods allow accessing backend-specific objects when needed
    // (e.g., for third-party library integration like ImGui)
    // Returns nullptr for non-matching backends

    virtual void* getBackendDevice() const { return nullptr; }
    // Backend texture object for third-party integration (ImGui previews):
    // MTL::Texture* on Metal, VkImageView on Vulkan.
    virtual void* getBackendTexture(TextureHandle /*handle*/) const { return nullptr; }
    // VkSampler on Vulkan (ImGui_ImplVulkan_AddTexture); null on Metal.
    virtual void* getBackendSampler(SamplerHandle /*handle*/) const { return nullptr; }
    virtual void* getBackendPhysicalDevice() const { return nullptr; }
    virtual void* getBackendInstance() const { return nullptr; }
    virtual void* getBackendQueue() const { return nullptr; }
    virtual void* getBackendCommandBuffer() const { return nullptr; }

    // Type-safe template wrappers
    template<typename T>
    T* getBackendDeviceAs() const {
        return static_cast<T*>(getBackendDevice());
    }

    template<typename T>
    T* getBackendPhysicalDeviceAs() const {
        return static_cast<T*>(getBackendPhysicalDevice());
    }

    template<typename T>
    T* getBackendInstanceAs() const {
        return static_cast<T*>(getBackendInstance());
    }

    template<typename T>
    T* getBackendQueueAs() const {
        return static_cast<T*>(getBackendQueue());
    }

    template<typename T>
    T* getBackendCommandBufferAs() const {
        return static_cast<T*>(getBackendCommandBuffer());
    }
};

// ============================================================================
// Factory Functions
// ============================================================================

RHI* createRHIVulkan();
RHI* createRHIMetal();
