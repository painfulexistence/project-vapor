#pragma once
#include <SDL3/SDL_stdinc.h>
#include <cstdint>
#include <vector>

// ============================================================================
// RHI (Render Hardware Interface) Layer
//
// This layer provides a thin abstraction over GPU APIs (Vulkan, Metal, etc.)
// It manages GPU resources and command recording, but does NOT contain
// high-level rendering logic.
// ============================================================================

// Forward declarations
struct SDL_Window;
namespace glm {
    struct vec2;
    struct vec3;
    struct vec4;
    struct mat4;
}

// ============================================================================
// Handle Types
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
    Depth32Float,
    Depth24Stencil8,
};

enum class TextureUsage {
    Sampled,
    Storage,
    RenderTarget,
    DepthStencil,
};

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
    Uint32 arrayLayers = 1;
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
    bool depthTest = true;
    bool depthWrite = true;
    CompareOp depthCompareOp = CompareOp::Less;
    CullMode cullMode = CullMode::Back;
    bool frontFaceCounterClockwise = true;
    Uint32 sampleCount = 1;
    // TODO: Add descriptor set layouts when needed
};

struct RenderPassDesc {
    // Color attachments
    std::vector<TextureHandle> colorAttachments;
    std::vector<TextureHandle> resolveAttachments;

    // Depth attachment
    TextureHandle depthAttachment;

    // Clear values
    std::vector<glm::vec4> clearColors;
    float clearDepth = 1.0f;
    Uint32 clearStencil = 0;

    // Load/store operations (true = load, false = clear)
    std::vector<bool> loadColor;
    bool loadDepth = false;
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

    // ========================================================================
    // Resource Updates
    // ========================================================================

    virtual void updateBuffer(BufferHandle handle, const void* data, size_t offset, size_t size) = 0;
    virtual void updateTexture(TextureHandle handle, const void* data, size_t size) = 0;

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

    virtual void draw(Uint32 vertexCount, Uint32 instanceCount = 1, Uint32 firstVertex = 0, Uint32 firstInstance = 0) = 0;
    virtual void drawIndexed(Uint32 indexCount, Uint32 instanceCount = 1, Uint32 firstIndex = 0, int32_t vertexOffset = 0, Uint32 firstInstance = 0) = 0;

    // ========================================================================
    // Utility
    // ========================================================================

    virtual Uint32 getSwapchainWidth() const = 0;
    virtual Uint32 getSwapchainHeight() const = 0;
    virtual PixelFormat getSwapchainFormat() const = 0;
};

// ============================================================================
// Factory Functions
// ============================================================================

RHI* createRHIVulkan();
RHI* createRHIMetal();
