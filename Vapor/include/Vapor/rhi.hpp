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

// GPU-encoded command list (Metal MTLIndirectCommandBuffer). A compute kernel
// writes draw commands into it; the render pass replays the whole list with one
// executeICB call. Vulkan has no portable equivalent exposed here (its native
// multi-draw indirect already replays N draws in one call).
struct IndirectCommandBufferHandle {
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
    Task,  // mesh-shading amplification stage (Metal: object function)
    Mesh,  // mesh-shading geometry stage (Metal: mesh function)
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

// Component swizzle for texture views (currently just what debug previews need).
enum class TextureSwizzle {
    Identity,  // rgba -> rgba
    RRR1,      // r -> rgb, 1 -> a  (single-channel depth/AO rendered as grayscale)
};

// A lightweight view over an existing texture: reinterprets a layer range and/or
// applies a component swizzle without copying. Used for ImGui debug previews
// (grayscale single-channel RTs, one PSSM cascade layer of an array as 2D).
struct TextureViewDesc {
    TextureHandle source;
    Uint32 baseArrayLayer = 0;
    Uint32 layerCount = 1;
    TextureSwizzle swizzle = TextureSwizzle::Identity;
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
    // Metal: specialize the function with kBindlessMaterials=true (function
    // constant 0) — the PBR fragment then reads material textures from the
    // argument table at buffer(13) instead of the bound slots 0-5. Only set for
    // shaders that declare the constant (3d_pbr_normal_mapped.metal); ignored
    // on Vulkan.
    bool bindlessMaterials = false;
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

// Mesh-shading pipeline: task (optional) + mesh + fragment stages, no vertex
// input (the mesh shader pulls geometry from storage buffers itself). Vulkan
// VK_EXT_mesh_shader; Metal object/mesh functions. Only usable when
// RHICapabilities::meshShaders is true.
struct MeshPipelineDesc {
    ShaderHandle taskShader;      // invalid handle = mesh-only pipeline
    ShaderHandle meshShader;
    ShaderHandle fragmentShader;
    // Threadgroup sizes, needed by Metal's drawMeshThreadgroups (Vulkan bakes
    // them in the shaders' local_size). Must match the shaders.
    Uint32 taskThreadgroupSize = 32;
    Uint32 meshThreadgroupSize = 64;
    // Metal object/mesh amplification limits (Vulkan ignores these — it bakes
    // them into the SPIR-V). Without them Metal allocates no payload memory and
    // caps the mesh grid at 0, so the object->mesh handoff emits nothing.
    // payloadBytes must cover the task payload struct; maxMeshThreadgroupsPerObject
    // caps each object threadgroup's set_threadgroups_per_grid.
    Uint32 payloadBytes = 128;                 // MeshletPayload = uint[32]
    Uint32 maxMeshThreadgroupsPerObject = 32;  // <= task threads per group
    BlendMode blendMode = BlendMode::Opaque;
    bool depthTest = true;
    bool depthWrite = true;
    CompareOp depthCompareOp = CompareOp::Less;
    bool hasDepthAttachment = true;
    PixelFormat depthAttachmentFormat = PixelFormat::Depth32Float;
    CullMode cullMode = CullMode::Back;
    bool frontFaceCounterClockwise = true;
    Uint32 sampleCount = 1;
    std::vector<PixelFormat> colorAttachmentFormats = { PixelFormat::Swapchain };
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
    // Metal: pipeline may be referenced by indirect command buffer draws
    // (MTLRenderPipelineDescriptor.supportIndirectCommandBuffers). Required for
    // executeICB with an inherit-pipeline ICB. Ignored on Vulkan.
    bool supportsICB = false;
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
    // vkCmdDrawIndexedIndirect with drawCount > 1 (single-call multi-draw
    // indirect). Vulkan gates this on the multiDrawIndirect device feature; Metal
    // draws one indirect command per call, so it reports false.
    bool multiDrawIndirect = false;
    // Task + mesh shader pipelines (Vulkan VK_EXT_mesh_shader; Metal object/mesh
    // functions). Required for the meshlet-based GPU-driven path.
    bool meshShaders = false;
    // GPU-encoded indirect command buffers + bindless texture tables (Metal
    // MTLIndirectCommandBuffer + argument buffers). Required for the ICB draw
    // mode. Vulkan reports false: its native MDI is already a single call, and
    // device-generated commands aren't portably available (MoltenVK).
    bool indirectCommandBuffers = false;
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

    // Create a non-owning view (layer range + swizzle) over an existing texture.
    // Returns an invalid handle if the backend doesn't support it. destroyTexture
    // on the returned handle frees only the view, never the source's image.
    virtual TextureHandle createTextureView(const TextureViewDesc& /*desc*/) { return {}; }

    virtual ShaderHandle createShader(const ShaderDesc& desc) = 0;
    virtual void destroyShader(ShaderHandle handle) = 0;

    virtual SamplerHandle createSampler(const SamplerDesc& desc) = 0;
    virtual void destroySampler(SamplerHandle handle) = 0;

    virtual PipelineHandle createPipeline(const PipelineDesc& desc) = 0;
    virtual void destroyPipeline(PipelineHandle handle) = 0;
    // Mesh-shading pipeline (see MeshPipelineDesc). Returns an invalid handle on
    // backends without mesh-shader support; destroy with destroyPipeline.
    virtual PipelineHandle createMeshPipeline(const MeshPipelineDesc& /*desc*/) { return {}; }

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

    // Copy one mip level (layer 0) of src into one mip level of dst, recorded on
    // the current frame's command buffer (between passes, not inside one). src
    // and dst must be different textures; the two mip levels must have matching
    // dimensions. Used to stage a Hi-Z level through a scratch texture so the
    // pyramid can be built in a single sampleable texture without a read/write
    // feedback loop. Both images are left in a shader-readable state afterwards
    // (a copy's result is meant to be sampled next; setTexture does not itself
    // transition). Default no-op so backends without it still link.
    virtual void copyTexture(TextureHandle /*src*/, Uint32 /*srcMip*/,
                             TextureHandle /*dst*/, Uint32 /*dstMip*/) {}

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
    // Launch task-shader workgroups of the bound mesh pipeline (Vulkan
    // vkCmdDrawMeshTasksEXT; Metal drawMeshThreadgroups). No-op default so
    // backends without mesh shaders still link.
    virtual void drawMeshTasks(Uint32 /*groupCountX*/, Uint32 /*groupCountY*/ = 1, Uint32 /*groupCountZ*/ = 1) {}

    // ========================================================================
    // Indirect Command Buffers + bindless texture tables (Metal-only for now;
    // capabilities.indirectCommandBuffers gates all of it, defaults keep other
    // backends linking). See the ICB draw mode in the renderer.
    // ========================================================================

    // GPU-encodable command list holding up to maxCommands indexed draws. The
    // ICB inherits the encoder's pipeline and buffer bindings; commands carry
    // only indexCount/indexBuffer/instanceCount/baseVertex/baseInstance.
    virtual IndirectCommandBufferHandle createIndirectCommandBuffer(Uint32 /*maxCommands*/) { return {}; }
    virtual void destroyIndirectCommandBuffer(IndirectCommandBufferHandle /*handle*/) {}
    // Bind the ICB to the current COMPUTE pass so the kernel can encode into it
    // (Metal: container argument buffer at `binding` + useResource(write); the
    // kernel declares `device ICBContainer& { command_buffer icb; }`).
    virtual void bindComputeICB(Uint32 /*binding*/, IndirectCommandBufferHandle /*handle*/) {}
    // Replay commands [0, commandCount) of the ICB on the current render pass.
    // The bound pipeline must have been created with supportsICB.
    virtual void executeICB(IndirectCommandBufferHandle /*handle*/, Uint32 /*commandCount*/) {}

    // Bindless texture table: an argument buffer holding `entryCount` structs of
    // `texturesPerEntry` texture handles each, laid out to match the given
    // fragment shader's argument at `bufferIndex` (Metal argument encoder).
    // Write entries with writeTextureArgumentTable; call useArgumentTableResources
    // inside the render pass (after bindPipeline) to make every written texture
    // resident, then bind the table with setFragmentBuffer(bufferIndex, table).
    virtual BufferHandle createTextureArgumentTable(ShaderHandle /*fragmentShader*/, Uint32 /*bufferIndex*/,
                                                    Uint32 /*entryCount*/, Uint32 /*texturesPerEntry*/) { return {}; }
    virtual void writeTextureArgumentTable(BufferHandle /*table*/, Uint32 /*entry*/, Uint32 /*slot*/,
                                           TextureHandle /*texture*/) {}
    virtual void useArgumentTableResources(BufferHandle /*table*/) {}

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
    // Storage-image bind (imageLoad/imageStore, single mip, no filtering).
    virtual void setComputeTexture(Uint32 binding, TextureHandle texture) = 0;
    // Sampled bind: the compute shader reads via a sampler (textureLod), so it
    // can sample any mip of a mipped texture — needed for hierarchical Hi-Z.
    // Default no-op so backends without it still link; Vulkan/Metal override.
    virtual void setComputeSampledTexture(Uint32 /*binding*/, TextureHandle /*texture*/, SamplerHandle /*sampler*/) {}
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
    // Wall-clock span of one frame's GPU work (first sample -> last sample),
    // in ms. This is the frame's GPU LATENCY: with frames pipelined on the GPU
    // it legitimately exceeds the frame period (e.g. 20ms span at 90fps =
    // pipeline depth ~1.8). Not additive with adjacent frames.
    // 0.0 = backend doesn't report it.
    virtual double getGpuFrameSpanMs() { return 0.0; }
    // Approximate GPU occupancy for one frame, in ms: the interval-UNION of
    // all pass windows (overlap counted once, inter-pass gaps excluded). This
    // is the number to compare against the frame period — ~frame period means
    // GPU-bound, much less means the bottleneck is elsewhere (CPU/vsync).
    // 0.0 = backend doesn't report it.
    virtual double getGpuFrameBusyMs() { return 0.0; }

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
