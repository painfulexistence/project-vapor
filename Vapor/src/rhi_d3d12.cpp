// ============================================================================
// RHI D3D12 backend (Windows).
//
// Mirrors the Vulkan backend's renderer-visible semantics so renderer.cpp
// works unmodified: it consumes the SAME SPIR-V modules (createShader receives
// .spv bytes) and cross-compiles them at load — spirv-cross → HLSL → dxc →
// DXIL, with an on-disk cache — instead of adding a third shader corpus.
// spirv-cross's default register mapping (set→space, binding→register) is what
// the fixed root signatures below are built around; push constants are remapped
// to root constants at (b0, space15).
//
// Renderer-visible conventions shared with rhi_vulkan.cpp:
//  - graphics set 0 = vertex-stage SSBOs      → SRV t0-7 / UAV u0-7, space0
//  - graphics set 1 = fragment-stage SSBOs    → SRV t0-7 / UAV u0-7, space1
//  - graphics set 2 = combined image+sampler  → SRV t0-12 + sampler s0-12, space2
//  - graphics set 3 = bindless texture table  → SRV t0[] + static sampler s1, space3
//  - compute  set 0 = SSBOs                   → SRV t0-7 / UAV u0-7, space0
//  - compute  set 1 = storage images          → UAV u0-7, space1
//  - compute  set 2 = sampled textures        → SRV t0-7 + sampler s0-7, space2
//  - compute  set 3 = acceleration structures → SRV t0-7, space3 (future RT)
//  - compute  set 4 = bindless texture table  → SRV t0[] + static sampler s1, space4
//  - push constants: vertex bytes at (binding%4)*16 in [0,64), fragment bytes
//    at 64+(binding%4)*16, compute bytes at (binding%4)*16 in [0,64) — the
//    exact offsets rhi_vulkan.cpp uses, because the GLSL twins bake them in.
//  - Vulkan renders through a negative-height viewport (Y-up NDC), which is
//    exactly D3D12's convention — no shader Y-flip, no winding flip.
//
// Buffers rely on D3D12's implicit COMMON promotion/decay (buffers promote to
// any state on first use and decay at ExecuteCommandLists), so only textures
// carry tracked states; compute write→read hazards use UAV barriers.
// ============================================================================

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <directx/d3d12.h>  // official DirectX-Headers (newer than any SDK's)
#include <dxgi1_6.h>
#ifndef _MSC_VER
#include <dxguids/dxguids.h>  // IID constants without native __uuidof
#endif
#ifndef _MSC_VER
// MinGW's sal.h lacks some SAL2 annotations dxcapi.h uses.
#ifndef _Maybenull_
#define _Maybenull_
#endif
#ifndef _Outptr_result_maybenull_
#define _Outptr_result_maybenull_
#endif
#endif
#include <dxcapi.h>

#include "rhi.hpp"
#include "rhi_d3d12_imgui.hpp"

#include <SDL3/SDL.h>
#include <fmt/core.h>
#include <spirv_cross/spirv_hlsl.hpp>

#include "imgui.h"
#include "backends/imgui_impl_dx12.h"
#include "backends/imgui_impl_sdl3.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <functional>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace Vapor {

namespace {

// ----------------------------------------------------------------------------
// Small utilities
// ----------------------------------------------------------------------------

// Minimal COM smart pointer (avoids wrl/client.h, which MinGW lacks pieces of).
template <typename T>
struct DxPtr {
    T* p = nullptr;
    DxPtr() = default;
    DxPtr(const DxPtr&) = delete;
    DxPtr& operator=(const DxPtr&) = delete;
    DxPtr(DxPtr&& o) noexcept : p(o.p) { o.p = nullptr; }
    DxPtr& operator=(DxPtr&& o) noexcept {
        if (this != &o) { reset(); p = o.p; o.p = nullptr; }
        return *this;
    }
    ~DxPtr() { reset(); }
    void reset() {
        if (p) { p->Release(); p = nullptr; }
    }
    T** put() { reset(); return &p; }
    void** putVoid() { reset(); return reinterpret_cast<void**>(&p); }
    T* operator->() const { return p; }
    operator T*() const { return p; }
    explicit operator bool() const { return p != nullptr; }
};

// dxcapi.h assigns interface uuids via __declspec(uuid), which GCC ignores —
// explicit IIDs work under every toolchain.
constexpr IID kIID_IDxcUtils     = {0x4605C4CB, 0x2019, 0x492A, {0xAD, 0xA4, 0x65, 0xF2, 0x0B, 0xB7, 0xD6, 0x7F}};
constexpr IID kIID_IDxcCompiler3 = {0x228B4687, 0x5A6A, 0x4730, {0x90, 0x0C, 0x97, 0x02, 0xB2, 0x20, 0x3F, 0x54}};
constexpr IID kIID_IDxcResult    = {0x58346CDA, 0xDDE7, 0x4497, {0x94, 0x61, 0x6F, 0x87, 0xAF, 0x5E, 0x06, 0x59}};
constexpr IID kIID_IDxcBlob      = {0x8BA5FB08, 0x5195, 0x40E2, {0xAC, 0x58, 0x0D, 0x98, 0x9C, 0x3A, 0x01, 0x02}};
constexpr IID kIID_IDxcBlobUtf8  = {0x3DA636C9, 0xBA71, 0x4024, {0xA3, 0x01, 0x30, 0xCB, 0xF1, 0x25, 0x30, 0x5B}};

void throwIfFailed(HRESULT hr, const char* what) {
    if (FAILED(hr)) {
        throw std::runtime_error(fmt::format("RHI_D3D12: {} failed (hr=0x{:08x})", what, (unsigned)hr));
    }
}

Uint64 fnv1a64(const void* data, size_t size, Uint64 seed = 1469598103934665603ull) {
    const Uint8* b = static_cast<const Uint8*>(data);
    Uint64 h = seed;
    for (size_t i = 0; i < size; i++) {
        h ^= b[i];
        h *= 1099511628211ull;
    }
    return h;
}

constexpr size_t alignUp(size_t v, size_t a) { return (v + a - 1) & ~(a - 1); }

// MinGW's COM ABI can't return aggregates by value from virtuals; the official
// DirectX-Headers switch these three to an out-parameter there. Wrap both forms.
inline D3D12_CPU_DESCRIPTOR_HANDLE heapStartCpu(ID3D12DescriptorHeap* heap) {
#if defined(_MSC_VER)
    return heap->GetCPUDescriptorHandleForHeapStart();
#else
    D3D12_CPU_DESCRIPTOR_HANDLE r;
    heap->GetCPUDescriptorHandleForHeapStart(&r);
    return r;
#endif
}
inline D3D12_GPU_DESCRIPTOR_HANDLE heapStartGpu(ID3D12DescriptorHeap* heap) {
#if defined(_MSC_VER)
    return heap->GetGPUDescriptorHandleForHeapStart();
#else
    D3D12_GPU_DESCRIPTOR_HANDLE r;
    heap->GetGPUDescriptorHandleForHeapStart(&r);
    return r;
#endif
}
inline D3D12_RESOURCE_DESC resourceDesc(ID3D12Resource* resource) {
#if defined(_MSC_VER)
    return resource->GetDesc();
#else
    D3D12_RESOURCE_DESC r;
    resource->GetDesc(&r);
    return r;
#endif
}

// Storage format for a texture resource. Depth formats go typeless so the same
// resource serves DSV writes and SRV reads.
DXGI_FORMAT convertResourceFormat(PixelFormat format) {
    switch (format) {
        case PixelFormat::RGBA8_UNORM:     return DXGI_FORMAT_R8G8B8A8_UNORM;
        case PixelFormat::RGBA8_SRGB:      return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        case PixelFormat::RGBA16_FLOAT:    return DXGI_FORMAT_R16G16B16A16_FLOAT;
        case PixelFormat::RGBA32_FLOAT:    return DXGI_FORMAT_R32G32B32A32_FLOAT;
        case PixelFormat::BGRA8_UNORM:     return DXGI_FORMAT_B8G8R8A8_UNORM;
        case PixelFormat::BGRA8_SRGB:      return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        case PixelFormat::R8_UNORM:        return DXGI_FORMAT_R8_UNORM;
        case PixelFormat::R16_FLOAT:       return DXGI_FORMAT_R16_FLOAT;
        case PixelFormat::R32_FLOAT:       return DXGI_FORMAT_R32_FLOAT;
        case PixelFormat::RG32_FLOAT:      return DXGI_FORMAT_R32G32_FLOAT;
        case PixelFormat::RGB32_FLOAT:     return DXGI_FORMAT_R32G32B32_FLOAT;
        case PixelFormat::Depth32Float:    return DXGI_FORMAT_R32_TYPELESS;
        case PixelFormat::Depth24Stencil8: return DXGI_FORMAT_R24G8_TYPELESS;
        default:                           return DXGI_FORMAT_R8G8B8A8_UNORM;
    }
}

DXGI_FORMAT depthDsvFormat(PixelFormat format) {
    return format == PixelFormat::Depth24Stencil8 ? DXGI_FORMAT_D24_UNORM_S8_UINT
                                                  : DXGI_FORMAT_D32_FLOAT;
}

DXGI_FORMAT depthSrvFormat(PixelFormat format) {
    return format == PixelFormat::Depth24Stencil8 ? DXGI_FORMAT_R24_UNORM_X8_TYPELESS
                                                  : DXGI_FORMAT_R32_FLOAT;
}

bool isDepthFormat(PixelFormat format) {
    return format == PixelFormat::Depth32Float || format == PixelFormat::Depth24Stencil8;
}

// Vertex attribute format (input layout).
DXGI_FORMAT convertAttributeFormat(PixelFormat format) {
    switch (format) {
        case PixelFormat::R32_FLOAT:    return DXGI_FORMAT_R32_FLOAT;
        case PixelFormat::RG32_FLOAT:   return DXGI_FORMAT_R32G32_FLOAT;
        case PixelFormat::RGB32_FLOAT:  return DXGI_FORMAT_R32G32B32_FLOAT;
        case PixelFormat::RGBA32_FLOAT: return DXGI_FORMAT_R32G32B32A32_FLOAT;
        case PixelFormat::RGBA8_UNORM:  return DXGI_FORMAT_R8G8B8A8_UNORM;
        default:                        return convertResourceFormat(format);
    }
}

D3D12_FILTER convertFilter(const SamplerDesc& desc) {
    // D3D12 collapses min/mag/mip into one enum; anisotropy overrides all.
    if (desc.enableAnisotropy) {
        return desc.enableCompare ? D3D12_FILTER_COMPARISON_ANISOTROPIC : D3D12_FILTER_ANISOTROPIC;
    }
    int bits = (desc.minFilter == FilterMode::Linear ? 0x10 : 0)
             | (desc.magFilter == FilterMode::Linear ? 0x04 : 0)
             | (desc.mipFilter == FilterMode::Linear ? 0x01 : 0);
    return static_cast<D3D12_FILTER>(bits | (desc.enableCompare ? 0x80 : 0));
}

D3D12_TEXTURE_ADDRESS_MODE convertAddressMode(AddressMode mode) {
    switch (mode) {
        case AddressMode::ClampToEdge:   return D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        case AddressMode::ClampToBorder: return D3D12_TEXTURE_ADDRESS_MODE_BORDER;
        case AddressMode::MirrorRepeat:  return D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
        default:                         return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    }
}

D3D12_COMPARISON_FUNC convertCompareOp(CompareOp op) {
    switch (op) {
        case CompareOp::Never:          return D3D12_COMPARISON_FUNC_NEVER;
        case CompareOp::Less:           return D3D12_COMPARISON_FUNC_LESS;
        case CompareOp::Equal:          return D3D12_COMPARISON_FUNC_EQUAL;
        case CompareOp::LessOrEqual:    return D3D12_COMPARISON_FUNC_LESS_EQUAL;
        case CompareOp::Greater:        return D3D12_COMPARISON_FUNC_GREATER;
        case CompareOp::NotEqual:       return D3D12_COMPARISON_FUNC_NOT_EQUAL;
        case CompareOp::GreaterOrEqual: return D3D12_COMPARISON_FUNC_GREATER_EQUAL;
        default:                        return D3D12_COMPARISON_FUNC_ALWAYS;
    }
}

D3D_PRIMITIVE_TOPOLOGY convertTopology(PrimitiveTopology topo) {
    switch (topo) {
        case PrimitiveTopology::PointList:     return D3D_PRIMITIVE_TOPOLOGY_POINTLIST;
        case PrimitiveTopology::LineList:      return D3D_PRIMITIVE_TOPOLOGY_LINELIST;
        case PrimitiveTopology::LineStrip:     return D3D_PRIMITIVE_TOPOLOGY_LINESTRIP;
        case PrimitiveTopology::TriangleStrip: return D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
        default:                               return D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    }
}

D3D12_PRIMITIVE_TOPOLOGY_TYPE convertTopologyType(PrimitiveTopology topo) {
    switch (topo) {
        case PrimitiveTopology::PointList: return D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
        case PrimitiveTopology::LineList:
        case PrimitiveTopology::LineStrip: return D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
        default:                           return D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    }
}

D3D12_RENDER_TARGET_BLEND_DESC convertBlendMode(BlendMode mode) {
    // Factor-for-factor mirror of rhi_vulkan.cpp's convertBlendMode.
    D3D12_RENDER_TARGET_BLEND_DESC b{};
    b.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    b.BlendOp = D3D12_BLEND_OP_ADD;
    b.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    b.LogicOp = D3D12_LOGIC_OP_NOOP;
    switch (mode) {
        case BlendMode::AlphaBlend:
            b.BlendEnable = TRUE;
            b.SrcBlend = D3D12_BLEND_SRC_ALPHA;
            b.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
            b.SrcBlendAlpha = D3D12_BLEND_ONE;
            b.DestBlendAlpha = D3D12_BLEND_ZERO;
            break;
        case BlendMode::Additive:
            b.BlendEnable = TRUE;
            b.SrcBlend = D3D12_BLEND_ONE;
            b.DestBlend = D3D12_BLEND_ONE;
            b.SrcBlendAlpha = D3D12_BLEND_ONE;
            b.DestBlendAlpha = D3D12_BLEND_ONE;
            break;
        case BlendMode::Multiply:
            b.BlendEnable = TRUE;
            b.SrcBlend = D3D12_BLEND_DEST_COLOR;
            b.DestBlend = D3D12_BLEND_ZERO;
            b.SrcBlendAlpha = D3D12_BLEND_ONE;
            b.DestBlendAlpha = D3D12_BLEND_ZERO;
            break;
        default:
            b.BlendEnable = FALSE;
            b.SrcBlend = D3D12_BLEND_ONE;
            b.DestBlend = D3D12_BLEND_ZERO;
            b.SrcBlendAlpha = D3D12_BLEND_ONE;
            b.DestBlendAlpha = D3D12_BLEND_ZERO;
            break;
    }
    return b;
}

} // namespace

// ============================================================================
// RHI_D3D12
// ============================================================================

class RHI_D3D12 : public RHI {
public:
    ~RHI_D3D12() override;

    // ------------------------------------------------------------------------
    // Sizing constants (renderer-visible ones mirror rhi_vulkan.hpp)
    // ------------------------------------------------------------------------
    static constexpr Uint32 MAX_FRAMES_IN_FLIGHT = 3;
    static constexpr Uint32 SWAPCHAIN_BUFFERS = 3;
    static constexpr Uint32 BINDINGS_PER_SET = 8;         // sets 0/1 + compute
    static constexpr Uint32 TEXTURE_BINDINGS_PER_SET = 13; // graphics set 2
    static constexpr Uint32 BINDLESS_TABLE_CAPACITY = 30720;

    // Shader-visible CBV/SRV/UAV heap layout (descriptor indices):
    //   [0, BINDLESS_REGION)                  two persistent bindless tables
    //   [BINDLESS_REGION, +IMGUI_REGION)      ImGui font/preview SRVs
    //   [RING_BASE, +RING_PER_FRAME*3)        per-frame transient tables
    static constexpr Uint32 BINDLESS_REGION = BINDLESS_TABLE_CAPACITY * 2 + 64;
    static constexpr Uint32 IMGUI_REGION = 1024;
    static constexpr Uint32 RING_PER_FRAME = 65536;
    static constexpr Uint32 RING_BASE = BINDLESS_REGION + IMGUI_REGION;
    static constexpr Uint32 GPU_HEAP_SIZE = RING_BASE + RING_PER_FRAME * MAX_FRAMES_IN_FLIGHT;

    static constexpr Uint32 SAMPLER_HEAP_SIZE = 2048;  // hard D3D12 limit
    static constexpr Uint32 CPU_SRV_HEAP_SIZE = 65536;
    static constexpr Uint32 CPU_RTV_HEAP_SIZE = 4096;
    static constexpr Uint32 CPU_DSV_HEAP_SIZE = 512;

    static constexpr Uint32 TIMESTAMP_QUERY_CAPACITY = 1024;  // per frame slot
    static constexpr Uint32 MAX_TIMED_PASSES = TIMESTAMP_QUERY_CAPACITY / 2;

    // Graphics root parameter indices
    enum {
        kGfxRootSet0 = 0,      // t0-7 + u0-7, space0
        kGfxRootSet1,          // t0-7 + u0-7, space1
        kGfxRootTextures,      // t0-12, space2
        kGfxRootSamplers,      // s0-12, space2
        kGfxRootConstants,     // b0, space15 (32 DWORDs)
        kGfxRootBindless,      // t0-unbounded, space3
        kGfxRootBaseVertex,    // b1, space15 (2 DWORDs) — sub-SM6.8 gl_InstanceIndex fix
        kGfxRootCount
    };
    // Compute root parameter indices
    enum {
        kCmpRootSet0 = 0,      // t0-7 + u0-7, space0
        kCmpRootImages,        // u0-7, space1
        kCmpRootSampled,       // t0-7, space2
        kCmpRootSamplers,      // s0-7, space2
        kCmpRootConstants,     // b0, space15 (32 DWORDs)
        kCmpRootAccel,         // t0-7, space3
        kCmpRootBindless,      // t0-unbounded, space4
        kCmpRootCount
    };

    // ------------------------------------------------------------------------
    // RHI interface
    // ------------------------------------------------------------------------
    bool initialize(SDL_Window* window) override;
    void shutdown() override;
    void waitIdle() override;
    const RHICapabilities& getCapabilities() const override { return capabilities; }
    Uint32 getMaxFramesInFlight() const override { return MAX_FRAMES_IN_FLIGHT; }

    BufferHandle createBuffer(const BufferDesc& desc) override;
    void destroyBuffer(BufferHandle handle) override;
    TextureHandle createTexture(const TextureDesc& desc) override;
    void destroyTexture(TextureHandle handle) override;
    TextureHandle createTextureView(const TextureViewDesc& desc) override;
    ShaderHandle createShader(const ShaderDesc& desc) override;
    void destroyShader(ShaderHandle handle) override;
    SamplerHandle createSampler(const SamplerDesc& desc) override;
    void destroySampler(SamplerHandle handle) override;
    PipelineHandle createPipeline(const PipelineDesc& desc) override;
    void destroyPipeline(PipelineHandle handle) override;
    PipelineHandle createMeshPipeline(const MeshPipelineDesc& desc) override;
    ComputePipelineHandle createComputePipeline(const ComputePipelineDesc& desc) override;
    void destroyComputePipeline(ComputePipelineHandle handle) override;
    AccelStructHandle createAccelerationStructure(const AccelStructDesc& desc) override;
    void destroyAccelerationStructure(AccelStructHandle handle) override;
    void buildAccelerationStructure(AccelStructHandle handle) override;
    void updateAccelerationStructure(AccelStructHandle handle, const std::vector<AccelStructInstance>& instances) override;

    void updateBuffer(BufferHandle handle, const void* data, size_t offset, size_t size) override;
    void updateTexture(TextureHandle handle, const void* data, size_t size, Uint32 mipLevel, Uint32 arrayLayer) override;
    void generateMipmaps(TextureHandle handle) override;
    void copyTexture(TextureHandle src, Uint32 srcMip, TextureHandle dst, Uint32 dstMip) override;
    void flushUploads() override;
    BufferHandle copySwapchainToBuffer(Uint32& outWidth, Uint32& outHeight) override;
    void* mapBuffer(BufferHandle handle) override;
    void unmapBuffer(BufferHandle handle) override;

    void beginFrame() override;
    void endFrame() override;
    void beginRenderPass(const RenderPassDesc& desc) override;
    void endRenderPass() override;

    void bindPipeline(PipelineHandle pipeline) override;
    void bindVertexBuffer(BufferHandle buffer, Uint32 binding, size_t offset) override;
    void bindIndexBuffer(BufferHandle buffer, size_t offset) override;
    void setUniformBuffer(Uint32 set, Uint32 binding, BufferHandle buffer, size_t offset, size_t range) override;
    void setStorageBuffer(Uint32 set, Uint32 binding, BufferHandle buffer, size_t offset, size_t range) override;
    void setTexture(Uint32 set, Uint32 binding, TextureHandle texture, SamplerHandle sampler) override;
    void setVertexBuffer(Uint32 binding, BufferHandle buffer, size_t offset, size_t range) override;
    void setFragmentBuffer(Uint32 binding, BufferHandle buffer, size_t offset, size_t range) override;
    void setVertexBytes(const void* data, size_t size, Uint32 binding) override;
    void setFragmentBytes(const void* data, size_t size, Uint32 binding) override;

    void draw(Uint32 vertexCount, Uint32 instanceCount, Uint32 firstVertex, Uint32 firstInstance) override;
    void drawIndexed(Uint32 indexCount, Uint32 instanceCount, Uint32 firstIndex, int32_t vertexOffset, Uint32 firstInstance) override;
    void drawIndexedIndirect(BufferHandle argsBuffer, size_t offset, Uint32 drawCount, Uint32 stride) override;
    void drawIndirect(BufferHandle argsBuffer, size_t offset, Uint32 drawCount, Uint32 stride) override;
    void drawMeshTasks(Uint32 groupCountX, Uint32 groupCountY, Uint32 groupCountZ) override;
    void drawMeshTasksIndirect(BufferHandle argsBuffer, size_t offset) override;

    BufferHandle createTextureArgumentTable(ShaderHandle fragmentShader, Uint32 bufferIndex,
                                            Uint32 entryCount, Uint32 texturesPerEntry) override;
    void writeTextureArgumentTable(BufferHandle table, Uint32 entry, Uint32 slot, TextureHandle texture) override;
    void bindTextureArgumentTable(BufferHandle table) override;
    void bindComputeTextureArgumentTable(BufferHandle table, Uint32 bufferIndex) override;

    void beginComputePass(const char* name) override;
    void endComputePass() override;
    void bindComputePipeline(ComputePipelineHandle pipeline) override;
    void setComputeBuffer(Uint32 binding, BufferHandle buffer, size_t offset, size_t range) override;
    void setComputeTexture(Uint32 binding, TextureHandle texture) override;
    void setComputeSampledTexture(Uint32 binding, TextureHandle texture, SamplerHandle sampler) override;
    void setAccelerationStructure(Uint32 binding, AccelStructHandle accelStruct) override;
    void setComputeBytes(const void* data, size_t size, Uint32 binding) override;
    void dispatch(Uint32 groupCountX, Uint32 groupCountY, Uint32 groupCountZ) override;
    void dispatchIndirect(BufferHandle argsBuffer, size_t offset) override;
    void computeBarrier() override;
    void prepareTextureForSampling(TextureHandle texture) override;
    void setScissor(int32_t x, int32_t y, Uint32 width, Uint32 height) override;

    Uint32 getSwapchainWidth() const override { return swapchainWidth; }
    Uint32 getSwapchainHeight() const override { return swapchainHeight; }
    PixelFormat getSwapchainFormat() const override { return PixelFormat::BGRA8_SRGB; }

    bool isGpuTimingSupported() const override { return capabilities.gpuTimestamps; }
    void setGpuTimingEnabled(bool enabled) override { gpuTimingEnabled = enabled; }
    bool isGpuTimingEnabled() const override { return gpuTimingEnabled; }
    std::vector<GpuPassTiming> getGpuPassTimings() override;
    double getGpuFrameSpanMs() override { return lastFrameSpanMs; }
    double getGpuFrameBusyMs() override { return lastFrameBusyMs; }

    void* getBackendDevice() const override { return device.p; }
    void* getBackendTexture(TextureHandle handle) const override;
    void* getBackendQueue() const override { return queue.p; }
    void* getBackendCommandBuffer() const override { return frameList.p; }

    // ImGui glue (rhi_d3d12_imgui.hpp free functions call these)
    bool imguiInit(SDL_Window* window);
    void imguiRenderDrawData();
    Uint64 imguiTextureID(TextureHandle texture);
    void imguiAllocSrv(D3D12_CPU_DESCRIPTOR_HANDLE* outCpu, D3D12_GPU_DESCRIPTOR_HANDLE* outGpu);
    void imguiFreeSrv(D3D12_CPU_DESCRIPTOR_HANDLE cpu, D3D12_GPU_DESCRIPTOR_HANDLE gpu);

private:
    // ------------------------------------------------------------------------
    // Resource records
    // ------------------------------------------------------------------------
    struct BufferResource {
        DxPtr<ID3D12Resource> resource;
        size_t size = 0;            // requested size
        size_t allocSize = 0;       // 4-byte-aligned allocation the raw views cover
        BufferUsage usage = BufferUsage::Vertex;
        MemoryUsage memoryUsage = MemoryUsage::GPU;
        void* mapped = nullptr;     // persistent map for upload/readback heaps
        D3D12_GPU_VIRTUAL_ADDRESS va = 0;
        Uint32 srvIndex = UINT32_MAX;  // raw SRV in the CPU staging heap
        Uint32 uavIndex = UINT32_MAX;  // raw UAV ("" ; null descriptor when not UAV-capable)
        // Swapchain readback: rows land 256-aligned; mapBuffer() compacts them
        // tight once (renderer memcpy's width*height*4).
        Uint32 readbackRowPitch = 0;
        Uint32 readbackWidth = 0;
        Uint32 readbackHeight = 0;
        bool readbackCompacted = false;
    };

    struct TextureResource {
        DxPtr<ID3D12Resource> resource;   // empty for views (borrowed from source)
        TextureDesc desc{};
        DXGI_FORMAT resourceFormat = DXGI_FORMAT_UNKNOWN;
        D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;  // whole-resource tracked state
        Uint32 srvIndex = UINT32_MAX;     // whole-texture SRV (CPU staging heap)
        Uint32 uavIndex = UINT32_MAX;     // mip-0 UAV
        // Lazily-created per-(layer,mip) attachment views, keyed layer<<8|mip
        // (layer 0xFFFFFF = whole-resource view).
        std::unordered_map<Uint64, Uint32> rtvCache;
        std::unordered_map<Uint64, Uint32> dsvCache;
        std::unordered_map<Uint64, Uint32> mipSrvCache;  // per-(layer,mip) SRV for mipgen
        // View support (createTextureView)
        bool isView = false;
        Uint32 viewSourceId = 0;
        // ImGui preview slot in the imgui SRV region (lazy)
        Uint32 imguiSlot = UINT32_MAX;
    };

    struct ShaderResource {
        std::vector<Uint8> dxil;
        ShaderStage stage = ShaderStage::Vertex;
    };

    struct PipelineResource {
        DxPtr<ID3D12PipelineState> pso;
        D3D_PRIMITIVE_TOPOLOGY topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
        Uint32 vertexStride = 0;
        bool isMesh = false;
    };

    struct ComputePipelineResource {
        DxPtr<ID3D12PipelineState> pso;
    };

    struct SamplerResource {
        D3D12_SAMPLER_DESC desc{};
        Uint64 hash = 0;
    };

    struct AccelBuffer {
        DxPtr<ID3D12Resource> resource;
        size_t size = 0;
        void* mapped = nullptr;
    };

    struct AccelStructResource {
        static constexpr Uint32 kTlasSlots = MAX_FRAMES_IN_FLIGHT;
        AccelStructDesc desc;
        // BLAS: single result/scratch. TLAS: rotated result/scratch/instance
        // slots so a rebuild never writes a structure an in-flight frame reads.
        AccelBuffer result[kTlasSlots];
        AccelBuffer scratch[kTlasSlots];
        AccelBuffer instances[kTlasSlots];
        Uint32 currentSlot = 0;
        bool built = false;
        D3D12_GPU_VIRTUAL_ADDRESS currentVA() const { return result[currentSlot].resource ? result[currentSlot].resource->GetGPUVirtualAddress() : 0; }
    };

    struct ArgumentTable {
        Uint32 baseSlot = 0;   // first descriptor index in the bindless region
        Uint32 capacity = 0;   // entryCount * texturesPerEntry
        Uint32 texturesPerEntry = 0;
    };

    // ------------------------------------------------------------------------
    // Descriptor heaps
    // ------------------------------------------------------------------------
    struct CpuHeap {
        DxPtr<ID3D12DescriptorHeap> heap;
        Uint32 capacity = 0;
        Uint32 next = 0;
        Uint32 stride = 0;
        std::vector<Uint32> freeList;
        Uint32 alloc() {
            if (!freeList.empty()) {
                Uint32 i = freeList.back();
                freeList.pop_back();
                return i;
            }
            if (next >= capacity) {
                throw std::runtime_error("RHI_D3D12: CPU descriptor heap exhausted");
            }
            return next++;
        }
        void free(Uint32 index) {
            if (index != UINT32_MAX) freeList.push_back(index);
        }
        D3D12_CPU_DESCRIPTOR_HANDLE cpuAt(Uint32 index) const {
            D3D12_CPU_DESCRIPTOR_HANDLE h = heapStartCpu(heap);
            h.ptr += SIZE_T(index) * stride;
            return h;
        }
    };

    // ------------------------------------------------------------------------
    // Backend state
    // ------------------------------------------------------------------------
    SDL_Window* window = nullptr;
    HWND hwnd = nullptr;

    DxPtr<IDXGIFactory6> factory;
    DxPtr<IDXGIAdapter1> adapter;
    DxPtr<ID3D12Device> device;
    DxPtr<ID3D12Device5> device5;          // DXR entry points (may be null)
    DxPtr<ID3D12CommandQueue> queue;
    DxPtr<IDXGISwapChain3> swapchain;
    RHICapabilities capabilities{};
    D3D_SHADER_MODEL highestShaderModel = D3D_SHADER_MODEL_6_0;

    Uint32 swapchainWidth = 0;
    Uint32 swapchainHeight = 0;
    DxPtr<ID3D12Resource> backbuffers[SWAPCHAIN_BUFFERS];
    Uint32 backbufferRtv[SWAPCHAIN_BUFFERS] = {UINT32_MAX, UINT32_MAX, UINT32_MAX};
    D3D12_RESOURCE_STATES backbufferState[SWAPCHAIN_BUFFERS] = {};
    Uint32 backbufferIndex = 0;

    // Frame pacing
    DxPtr<ID3D12Fence> fence;
    HANDLE fenceEvent = nullptr;
    Uint64 nextFenceValue = 1;
    Uint64 frameFenceValues[MAX_FRAMES_IN_FLIGHT] = {};
    Uint32 frameIndex = 0;      // 0..MAX_FRAMES_IN_FLIGHT-1
    bool insideFrame = false;
    bool frameListOpen = false;

    DxPtr<ID3D12CommandAllocator> frameAllocators[MAX_FRAMES_IN_FLIGHT];
    DxPtr<ID3D12GraphicsCommandList> frameList;
    DxPtr<ID3D12GraphicsCommandList6> frameList6;   // DispatchMesh (may be null)
    DxPtr<ID3D12GraphicsCommandList4> frameList4;   // AS builds (may be null)

    // Upload stream: its own allocator+list, executed ahead of frame work.
    DxPtr<ID3D12CommandAllocator> uploadAllocator;
    DxPtr<ID3D12GraphicsCommandList> uploadList;
    bool uploadListOpen = false;
    std::vector<DxPtr<ID3D12Resource>*> uploadKeepAlive;  // staged buffers pending release

    // Deferred destruction: (fenceValue, resource) released once the GPU passes it.
    struct Zombie {
        Uint64 fenceValue;
        ID3D12Resource* resource;   // owned; Released on retire
    };
    // While the frame list is open, a deferred resource may be referenced by
    // commands already recorded into it (updateBuffer/updateTexture staging
    // copies, destroy of a resource drawn earlier in the frame). The frame's
    // signal value isn't known yet — loading-path submits (submitUploads,
    // executeImmediate) interleave and consume fence values, and their
    // retireZombies would free the resource before the frame list ever
    // executes (use-after-free -> DEVICE_HUNG). Stamp such zombies with a
    // sentinel no fence can reach, and re-stamp them in endFrame once the
    // frame's signal value is assigned. shutdown() releases unconditionally
    // after waitIdle, so a sentinel left at teardown cannot leak.
    static constexpr Uint64 kFencePendingFrame = UINT64_MAX;
    std::vector<Zombie> zombies;
    void deferRelease(ID3D12Resource* r) {
        if (r) zombies.push_back({frameListOpen ? kFencePendingFrame : nextFenceValue, r});
    }

    // Descriptor heaps
    CpuHeap cpuSrvHeap;   // CBV_SRV_UAV staging
    CpuHeap cpuRtvHeap;
    CpuHeap cpuDsvHeap;
    DxPtr<ID3D12DescriptorHeap> gpuSrvHeap;      // shader-visible CBV_SRV_UAV
    DxPtr<ID3D12DescriptorHeap> gpuSamplerHeap;  // shader-visible samplers
    Uint32 srvStride = 0;
    Uint32 samplerStride = 0;
    Uint32 ringCursor = 0;          // within this frame's RING_PER_FRAME slice
    Uint32 samplerHeapNext = 0;
    Uint32 bindlessNext = 0;        // persistent bindless region cursor
    Uint32 imguiNext = 0;           // imgui region cursor
    std::vector<Uint32> imguiFree;
    std::unordered_map<Uint64, Uint32> samplerTableCache;  // hash → first sampler slot

    D3D12_CPU_DESCRIPTOR_HANDLE gpuHeapCpuAt(Uint32 index) const {
        D3D12_CPU_DESCRIPTOR_HANDLE h = heapStartCpu(gpuSrvHeap.p);
        h.ptr += SIZE_T(index) * srvStride;
        return h;
    }
    D3D12_GPU_DESCRIPTOR_HANDLE gpuHeapGpuAt(Uint32 index) const {
        D3D12_GPU_DESCRIPTOR_HANDLE h = heapStartGpu(gpuSrvHeap.p);
        h.ptr += UINT64(index) * srvStride;
        return h;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE samplerHeapCpuAt(Uint32 index) const {
        D3D12_CPU_DESCRIPTOR_HANDLE h = heapStartCpu(gpuSamplerHeap.p);
        h.ptr += SIZE_T(index) * samplerStride;
        return h;
    }
    D3D12_GPU_DESCRIPTOR_HANDLE samplerHeapGpuAt(Uint32 index) const {
        D3D12_GPU_DESCRIPTOR_HANDLE h = heapStartGpu(gpuSamplerHeap.p);
        h.ptr += UINT64(index) * samplerStride;
        return h;
    }

    // Root signatures + command signatures
    DxPtr<ID3D12RootSignature> graphicsRootSig;
    DxPtr<ID3D12RootSignature> computeRootSig;
    std::unordered_map<Uint64, ID3D12CommandSignature*> commandSigCache;  // (kind<<32|stride)
    ID3D12CommandSignature* getCommandSignature(Uint32 kind, Uint32 stride);

    // dxc (runtime DXIL compile)
    HMODULE dxcompilerModule = nullptr;
    DxPtr<IDxcUtils> dxcUtils;
    DxPtr<IDxcCompiler3> dxcCompiler;
    std::filesystem::path shaderCacheDir;

    // Resource tables
    std::unordered_map<Uint32, BufferResource> buffers;
    std::unordered_map<Uint32, TextureResource> textures;
    std::unordered_map<Uint32, ShaderResource> shaders;
    std::unordered_map<Uint32, SamplerResource> samplers;
    std::unordered_map<Uint32, PipelineResource> pipelines;
    std::unordered_map<Uint32, ComputePipelineResource> computePipelines;
    std::unordered_map<Uint32, AccelStructResource> accelStructs;
    std::unordered_map<Uint32, ArgumentTable> argumentTables;
    Uint32 nextResourceId = 1;

    // ------------------------------------------------------------------------
    // Bind state (flushed into descriptor tables at draw/dispatch)
    // ------------------------------------------------------------------------
    struct BufferBinding {
        Uint32 id = 0;
        size_t offset = 0;
    };
    struct TextureBinding {
        Uint32 texId = 0;
        Uint32 samplerId = 0;
    };
    BufferBinding boundVertexBuffers[BINDINGS_PER_SET];
    BufferBinding boundFragmentBuffers[BINDINGS_PER_SET];
    TextureBinding boundTextures[TEXTURE_BINDINGS_PER_SET];
    Uint8 graphicsPushData[128] = {};
    bool graphicsDescriptorsDirty = true;
    bool graphicsPushDirty = true;
    Uint32 boundBindlessTable = 0;

    BufferBinding boundComputeBuffers[BINDINGS_PER_SET];
    Uint32 boundComputeImages[BINDINGS_PER_SET] = {};
    TextureBinding boundComputeSampled[BINDINGS_PER_SET];
    Uint32 boundComputeAccels[BINDINGS_PER_SET] = {};
    Uint8 computePushData[64] = {};
    bool computeDescriptorsDirty = true;
    bool computePushDirty = true;
    Uint32 boundComputeBindlessTable = 0;

    // Current pass / pipeline
    RenderPassDesc currentPassDesc;
    bool insideRenderPass = false;
    bool insideComputePass = false;
    Uint32 currentPipelineId = 0;
    Uint32 currentComputePipelineId = 0;
    Uint32 passWidth = 0;
    Uint32 passHeight = 0;
    struct PendingVB {
        Uint32 bufferId = 0;
        size_t offset = 0;
    };
    PendingVB pendingVertexBuffers[4];
    Uint32 pendingIndexBuffer = 0;
    size_t pendingIndexOffset = 0;
    bool vertexStreamDirty = false;

    // GPU timings
    bool gpuTimingEnabled = false;
    DxPtr<ID3D12QueryHeap> timestampHeap;
    DxPtr<ID3D12Resource> timestampReadback;   // TIMESTAMP_QUERY_CAPACITY * 3 UINT64s
    Uint64 timestampFrequency = 0;
    struct TimedPass {
        std::string name;
        Uint32 beginQuery;
        Uint32 endQuery;
    };
    std::vector<TimedPass> framePasses[MAX_FRAMES_IN_FLIGHT];
    Uint32 nextQuery = 0;   // within this frame's slice
    std::vector<GpuPassTiming> resolvedTimings;
    double lastFrameSpanMs = 0.0;
    double lastFrameBusyMs = 0.0;
    void beginPassTiming(const char* name);
    void endPassTiming();
    Uint32 openPassQuery = UINT32_MAX;
    void resolveTimings();

    // ImGui
    bool imguiInitialized = false;

    // ------------------------------------------------------------------------
    // Helpers
    // ------------------------------------------------------------------------
    ID3D12GraphicsCommandList* activeList();          // frame list if open, else upload list
    ID3D12GraphicsCommandList* ensureUploadList();
    void submitUploads(bool wait);
    void executeImmediate(const std::function<void(ID3D12GraphicsCommandList*)>& record);
    void waitForFenceValue(Uint64 value);
    void retireZombies(Uint64 completed);
    void transitionTexture(TextureResource& tex, D3D12_RESOURCE_STATES newState,
                           ID3D12GraphicsCommandList* list);
    TextureResource* resolveTexture(Uint32 id);       // follows views to their source
    Uint32 getOrCreateRTV(TextureResource& tex, Uint32 layer, Uint32 mip);
    Uint32 getOrCreateDSV(TextureResource& tex, Uint32 layer);
    Uint32 getOrCreateMipSrv(TextureResource& tex, Uint32 layer, Uint32 mip);
    void createBufferViews(BufferResource& buf);
    Uint32 allocRing(Uint32 count);                   // contiguous transient descriptors
    void writeNullSrv(Uint32 dstIndex);
    void writeNullUav(Uint32 dstIndex);
    void writeBufferSrv(Uint32 dstIndex, const BufferBinding& binding);
    void writeBufferUav(Uint32 dstIndex, const BufferBinding& binding);
    Uint32 getSamplerTable(const Uint32* samplerIds, Uint32 count);
    void flushGraphicsState();
    void flushComputeState();
    // Sub-SM6.8: gl_InstanceIndex/gl_VertexIndex are rebuilt from these root
    // constants (SM6.8 shaders read SV_StartInstanceLocation instead).
    void setBaseVertexConstants(int32_t baseVertex, Uint32 baseInstance) {
        if (highestShaderModel >= D3D_SHADER_MODEL_6_8) return;
        const int32_t values[2] = {baseVertex, static_cast<int32_t>(baseInstance)};
        frameList->SetGraphicsRoot32BitConstants(kGfxRootBaseVertex, 2, values, 0);
    }
    void ensureSwapchain();
    std::vector<Uint8> compileSpirvToDxil(const void* spirv, size_t size, ShaderStage stage);
    void createMipgenPipeline();
    DxPtr<ID3D12RootSignature> mipgenRootSig;
    std::unordered_map<Uint32, ID3D12PipelineState*> mipgenPsoCache;  // DXGI_FORMAT → PSO (owned)
    ID3D12PipelineState* getMipgenPso(DXGI_FORMAT rtvFormat);
    std::vector<Uint8> mipgenVsDxil, mipgenPsDxil;
    void buildAccelStructInternal(AccelStructResource& as, ID3D12GraphicsCommandList4* list, Uint32 slot);
    void ensureAccelBuffer(AccelBuffer& buf, size_t size, bool asResult, bool mappable);
};

// ============================================================================
// Initialization
// ============================================================================

bool RHI_D3D12::initialize(SDL_Window* sdlWindow) {
    window = sdlWindow;
    hwnd = static_cast<HWND>(SDL_GetPointerProperty(SDL_GetWindowProperties(window),
                                                    SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
    if (!hwnd) {
        fmt::print(stderr, "RHI_D3D12: SDL window has no Win32 HWND\n");
        return false;
    }

    UINT factoryFlags = 0;
#ifndef NDEBUG
    {
        DxPtr<ID3D12Debug> debug;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(debug.put())))) {
            debug->EnableDebugLayer();
            factoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
            // GPU-based validation instruments the shaders themselves, catching
            // what the CPU-side layer cannot: out-of-bounds descriptor and
            // buffer access, resource state mismatches seen by the GPU. Costs
            // roughly an order of magnitude in frame time, so it is opt-in.
            if (const char* gbv = std::getenv("VAPOR_D3D12_GBV"); gbv && gbv[0] == '1') {
                DxPtr<ID3D12Debug1> debug1;
                if (SUCCEEDED(debug->QueryInterface(IID_PPV_ARGS(debug1.put())))) {
                    debug1->SetEnableGPUBasedValidation(TRUE);
                    debug1->SetEnableSynchronizedCommandQueueValidation(TRUE);
                    fmt::print(stderr, "RHI_D3D12: GPU-based validation enabled\n");
                }
            }
        }
    }
#endif
    throwIfFailed(CreateDXGIFactory2(factoryFlags, IID_PPV_ARGS(factory.put())), "CreateDXGIFactory2");

    // Highest-performance adapter that accepts FL 12_0.
    for (UINT i = 0;; i++) {
        DxPtr<IDXGIAdapter1> candidate;
        if (factory->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                                IID_PPV_ARGS(candidate.put())) == DXGI_ERROR_NOT_FOUND) {
            break;
        }
        DXGI_ADAPTER_DESC1 desc{};
        candidate->GetDesc1(&desc);
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
        if (SUCCEEDED(D3D12CreateDevice(candidate, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(device.put())))) {
            adapter = std::move(candidate);
            break;
        }
    }
    if (!device) {
        fmt::print(stderr, "RHI_D3D12: no D3D12 feature-level 12_0 adapter found\n");
        return false;
    }
    device->QueryInterface(IID_PPV_ARGS(device5.put()));  // optional (DXR)

#ifndef NDEBUG
    // EnableDebugLayer alone reports through OutputDebugString, which nothing
    // sees without a debugger attached — so validation errors are invisible in a
    // plain terminal run and in CI. Mirror them to stderr instead. The callback
    // dies with the info queue, so the teardown live-object dump still only goes
    // to OutputDebugString.
    if (DxPtr<ID3D12InfoQueue1> infoQueue;
        SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(infoQueue.put())))) {
        // Clear-value mismatch is a performance note, not a correctness error,
        // and clear colors here are runtime data (per-pass RenderPassDesc,
        // including one editable in the Engine window) — no single optimized
        // clear value chosen at resource creation can match them all. Filter
        // the two IDs rather than pretending we could.
        D3D12_MESSAGE_ID denied[] = {
            D3D12_MESSAGE_ID_CLEARRENDERTARGETVIEW_MISMATCHINGCLEARVALUE,
            D3D12_MESSAGE_ID_CLEARDEPTHSTENCILVIEW_MISMATCHINGCLEARVALUE,
        };
        D3D12_INFO_QUEUE_FILTER filter{};
        filter.DenyList.NumIDs = static_cast<UINT>(std::size(denied));
        filter.DenyList.pIDList = denied;
        infoQueue->AddStorageFilterEntries(&filter);

        DWORD cookie = 0;
        infoQueue->RegisterMessageCallback(
            [](D3D12_MESSAGE_CATEGORY, D3D12_MESSAGE_SEVERITY severity, D3D12_MESSAGE_ID id,
               LPCSTR description, void*) {
                const char* level = nullptr;
                switch (severity) {
                    case D3D12_MESSAGE_SEVERITY_CORRUPTION: level = "CORRUPTION"; break;
                    case D3D12_MESSAGE_SEVERITY_ERROR:      level = "ERROR";      break;
                    case D3D12_MESSAGE_SEVERITY_WARNING:    level = "WARNING";    break;
                    default: return;  // INFO/MESSAGE are per-call chatter
                }
                fmt::print(stderr, "D3D12 {} #{}: {}\n", level, static_cast<int>(id), description);
            },
            D3D12_MESSAGE_CALLBACK_FLAG_NONE, nullptr, &cookie);
    }
#endif

    // Feature detection → RHICapabilities
    D3D12_FEATURE_DATA_SHADER_MODEL smQuery{ D3D_SHADER_MODEL_6_8 };
    if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &smQuery, sizeof(smQuery)))) {
        smQuery.HighestShaderModel = D3D_SHADER_MODEL_6_0;
        for (D3D_SHADER_MODEL probe : { D3D_SHADER_MODEL_6_7, D3D_SHADER_MODEL_6_6, D3D_SHADER_MODEL_6_5 }) {
            D3D12_FEATURE_DATA_SHADER_MODEL q{ probe };
            if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &q, sizeof(q)))) {
                smQuery = q;
                break;
            }
        }
    }
    highestShaderModel = smQuery.HighestShaderModel;

    D3D12_FEATURE_DATA_D3D12_OPTIONS5 opt5{};
    device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &opt5, sizeof(opt5));
    D3D12_FEATURE_DATA_D3D12_OPTIONS7 opt7{};
    device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7, &opt7, sizeof(opt7));

    capabilities.computeShaders = true;
    capabilities.gpuTimestamps = true;
    // Inline ray queries (the renderer's RT model) need DXR tier 1.1 + SM 6.5.
    capabilities.raytracing = device5 && opt5.RaytracingTier >= D3D12_RAYTRACING_TIER_1_1 &&
                              highestShaderModel >= D3D_SHADER_MODEL_6_5;
    // ExecuteIndirect always multi-draws; the gate is gl_InstanceIndex fidelity:
    // per-command firstInstance only reaches the shader through SM 6.8's
    // SV_StartInstanceLocation (the compensation cbuffer can't know GPU-written
    // values). DrawCommand layout == D3D12_DRAW_INDEXED_ARGUMENTS, no repack.
    capabilities.multiDrawIndirect = highestShaderModel >= D3D_SHADER_MODEL_6_8;
    capabilities.meshShaders = opt7.MeshShaderTier >= D3D12_MESH_SHADER_TIER_1 &&
                               highestShaderModel >= D3D_SHADER_MODEL_6_5;
    capabilities.indirectCommandBuffers = false;  // Metal-only by design
    capabilities.bindlessTextures = capabilities.multiDrawIndirect;  // bindless MDI mode needs both

    // In CI (no GPU) a WARP-less run never gets here; nothing to guard.

    D3D12_COMMAND_QUEUE_DESC queueDesc{};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    throwIfFailed(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(queue.put())), "CreateCommandQueue");
    queue->GetTimestampFrequency(&timestampFrequency);

    throwIfFailed(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(fence.put())), "CreateFence");
    fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!fenceEvent) return false;

    for (Uint32 i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        throwIfFailed(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                     IID_PPV_ARGS(frameAllocators[i].put())),
                      "CreateCommandAllocator");
    }
    throwIfFailed(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, frameAllocators[0],
                                            nullptr, IID_PPV_ARGS(frameList.put())),
                  "CreateCommandList");
    frameList->Close();
    frameList->QueryInterface(IID_PPV_ARGS(frameList6.put()));  // optional (mesh)
    frameList->QueryInterface(IID_PPV_ARGS(frameList4.put()));  // optional (DXR)
    if (!frameList6) capabilities.meshShaders = false;
    if (!frameList4) capabilities.raytracing = false;

    throwIfFailed(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                 IID_PPV_ARGS(uploadAllocator.put())),
                  "CreateCommandAllocator(upload)");
    throwIfFailed(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, uploadAllocator,
                                            nullptr, IID_PPV_ARGS(uploadList.put())),
                  "CreateCommandList(upload)");
    uploadList->Close();

    // --- Descriptor heaps --------------------------------------------------
    auto makeCpuHeap = [&](CpuHeap& out, D3D12_DESCRIPTOR_HEAP_TYPE type, Uint32 capacity) {
        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type = type;
        hd.NumDescriptors = capacity;
        throwIfFailed(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(out.heap.put())), "CreateDescriptorHeap");
        out.capacity = capacity;
        out.stride = device->GetDescriptorHandleIncrementSize(type);
    };
    makeCpuHeap(cpuSrvHeap, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, CPU_SRV_HEAP_SIZE);
    makeCpuHeap(cpuRtvHeap, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, CPU_RTV_HEAP_SIZE);
    makeCpuHeap(cpuDsvHeap, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, CPU_DSV_HEAP_SIZE);

    {
        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.NumDescriptors = GPU_HEAP_SIZE;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        throwIfFailed(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(gpuSrvHeap.put())), "gpu SRV heap");
        srvStride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
        hd.NumDescriptors = SAMPLER_HEAP_SIZE;
        throwIfFailed(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(gpuSamplerHeap.put())), "sampler heap");
        samplerStride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
    }

    // --- Root signatures ---------------------------------------------------
    auto makeTable = [](D3D12_DESCRIPTOR_RANGE1* ranges, Uint32 rangeCount) {
        D3D12_ROOT_PARAMETER1 p{};
        p.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        p.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        p.DescriptorTable.NumDescriptorRanges = rangeCount;
        p.DescriptorTable.pDescriptorRanges = ranges;
        return p;
    };
    auto makeRange = [](D3D12_DESCRIPTOR_RANGE_TYPE type, Uint32 count, Uint32 baseReg, Uint32 space,
                        Uint32 tableOffset) {
        D3D12_DESCRIPTOR_RANGE1 r{};
        r.RangeType = type;
        r.NumDescriptors = count;
        r.BaseShaderRegister = baseReg;
        r.RegisterSpace = space;
        // Volatile: descriptors are written after the table pointer is set
        // (ring copies happen right before the draw, but the debug layer's
        // static-descriptor validation is stricter than our timeline).
        r.Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE;
        r.OffsetInDescriptorsFromTableStart = tableOffset;
        return r;
    };

    D3D12_STATIC_SAMPLER_DESC bindlessSampler{};
    bindlessSampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    bindlessSampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    bindlessSampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    bindlessSampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    bindlessSampler.MaxLOD = D3D12_FLOAT32_MAX;
    bindlessSampler.ShaderRegister = 1;  // GLSL set3/binding1 sampler → s1
    bindlessSampler.RegisterSpace = 3;
    bindlessSampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    {
        // Graphics
        D3D12_DESCRIPTOR_RANGE1 set0Ranges[2] = {
            makeRange(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, BINDINGS_PER_SET, 0, 0, 0),
            makeRange(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, BINDINGS_PER_SET, 0, 0, BINDINGS_PER_SET),
        };
        D3D12_DESCRIPTOR_RANGE1 set1Ranges[2] = {
            makeRange(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, BINDINGS_PER_SET, 0, 1, 0),
            makeRange(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, BINDINGS_PER_SET, 0, 1, BINDINGS_PER_SET),
        };
        D3D12_DESCRIPTOR_RANGE1 texRange = makeRange(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, TEXTURE_BINDINGS_PER_SET, 0, 2, 0);
        D3D12_DESCRIPTOR_RANGE1 samplerRange = makeRange(D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, TEXTURE_BINDINGS_PER_SET, 0, 2, 0);
        D3D12_DESCRIPTOR_RANGE1 bindlessRange = makeRange(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, BINDLESS_TABLE_CAPACITY, 0, 3, 0);

        D3D12_ROOT_PARAMETER1 params[kGfxRootCount] = {};
        params[kGfxRootSet0] = makeTable(set0Ranges, 2);
        params[kGfxRootSet1] = makeTable(set1Ranges, 2);
        params[kGfxRootTextures] = makeTable(&texRange, 1);
        params[kGfxRootSamplers] = makeTable(&samplerRange, 1);
        params[kGfxRootConstants].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[kGfxRootConstants].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params[kGfxRootConstants].Constants.ShaderRegister = 0;
        params[kGfxRootConstants].Constants.RegisterSpace = 15;
        params[kGfxRootConstants].Constants.Num32BitValues = 32;
        params[kGfxRootBindless] = makeTable(&bindlessRange, 1);
        // Sub-SM6.8 devices: spirv-cross's SPIRV_Cross_VertexInfo compensation
        // cbuffer (BaseVertex, BaseInstance), fed per draw call. SM6.8 shaders
        // use SV_StartInstanceLocation natively and never reference it.
        params[kGfxRootBaseVertex].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[kGfxRootBaseVertex].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
        params[kGfxRootBaseVertex].Constants.ShaderRegister = 1;
        params[kGfxRootBaseVertex].Constants.RegisterSpace = 15;
        params[kGfxRootBaseVertex].Constants.Num32BitValues = 2;

        D3D12_VERSIONED_ROOT_SIGNATURE_DESC rs{};
        rs.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
        rs.Desc_1_1.NumParameters = kGfxRootCount;
        rs.Desc_1_1.pParameters = params;
        rs.Desc_1_1.NumStaticSamplers = 1;
        rs.Desc_1_1.pStaticSamplers = &bindlessSampler;
        rs.Desc_1_1.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

        DxPtr<ID3DBlob> blob, error;
        HRESULT hr = D3D12SerializeVersionedRootSignature(&rs, blob.put(), error.put());
        if (FAILED(hr)) {
            fmt::print(stderr, "RHI_D3D12: graphics root signature: {}\n",
                       error ? static_cast<const char*>(error->GetBufferPointer()) : "?");
            return false;
        }
        throwIfFailed(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                                  IID_PPV_ARGS(graphicsRootSig.put())),
                      "CreateRootSignature(graphics)");
    }
    {
        // Compute
        D3D12_DESCRIPTOR_RANGE1 set0Ranges[2] = {
            makeRange(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, BINDINGS_PER_SET, 0, 0, 0),
            makeRange(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, BINDINGS_PER_SET, 0, 0, BINDINGS_PER_SET),
        };
        D3D12_DESCRIPTOR_RANGE1 imageRange = makeRange(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, BINDINGS_PER_SET, 0, 1, 0);
        D3D12_DESCRIPTOR_RANGE1 sampledRange = makeRange(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, BINDINGS_PER_SET, 0, 2, 0);
        D3D12_DESCRIPTOR_RANGE1 samplerRange = makeRange(D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, BINDINGS_PER_SET, 0, 2, 0);
        D3D12_DESCRIPTOR_RANGE1 accelRange = makeRange(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, BINDINGS_PER_SET, 0, 3, 0);
        D3D12_DESCRIPTOR_RANGE1 bindlessRange = makeRange(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, BINDLESS_TABLE_CAPACITY, 0, 4, 0);

        D3D12_STATIC_SAMPLER_DESC computeBindlessSampler = bindlessSampler;
        computeBindlessSampler.RegisterSpace = 4;

        D3D12_ROOT_PARAMETER1 params[kCmpRootCount] = {};
        params[kCmpRootSet0] = makeTable(set0Ranges, 2);
        params[kCmpRootImages] = makeTable(&imageRange, 1);
        params[kCmpRootSampled] = makeTable(&sampledRange, 1);
        params[kCmpRootSamplers] = makeTable(&samplerRange, 1);
        params[kCmpRootConstants].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[kCmpRootConstants].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params[kCmpRootConstants].Constants.ShaderRegister = 0;
        params[kCmpRootConstants].Constants.RegisterSpace = 15;
        params[kCmpRootConstants].Constants.Num32BitValues = 32;
        params[kCmpRootAccel] = makeTable(&accelRange, 1);
        params[kCmpRootBindless] = makeTable(&bindlessRange, 1);

        D3D12_VERSIONED_ROOT_SIGNATURE_DESC rs{};
        rs.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
        rs.Desc_1_1.NumParameters = kCmpRootCount;
        rs.Desc_1_1.pParameters = params;
        rs.Desc_1_1.NumStaticSamplers = 1;
        rs.Desc_1_1.pStaticSamplers = &computeBindlessSampler;

        DxPtr<ID3DBlob> blob, error;
        HRESULT hr = D3D12SerializeVersionedRootSignature(&rs, blob.put(), error.put());
        if (FAILED(hr)) {
            fmt::print(stderr, "RHI_D3D12: compute root signature: {}\n",
                       error ? static_cast<const char*>(error->GetBufferPointer()) : "?");
            return false;
        }
        throwIfFailed(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                                  IID_PPV_ARGS(computeRootSig.put())),
                      "CreateRootSignature(compute)");
    }

    // --- dxc ---------------------------------------------------------------
    // Loaded at runtime so a missing redistributable produces a clear error
    // instead of a loader failure before main().
    dxcompilerModule = LoadLibraryW(L"dxcompiler.dll");
    if (!dxcompilerModule) {
        fmt::print(stderr, "RHI_D3D12: dxcompiler.dll not found next to the executable\n");
        return false;
    }
    auto dxcCreate = reinterpret_cast<DxcCreateInstanceProc>(
        reinterpret_cast<void*>(GetProcAddress(dxcompilerModule, "DxcCreateInstance")));
    if (!dxcCreate ||
        FAILED(dxcCreate(CLSID_DxcUtils, kIID_IDxcUtils, dxcUtils.putVoid())) ||
        FAILED(dxcCreate(CLSID_DxcCompiler, kIID_IDxcCompiler3, dxcCompiler.putVoid()))) {
        fmt::print(stderr, "RHI_D3D12: DxcCreateInstance failed\n");
        return false;
    }
    shaderCacheDir = std::filesystem::temp_directory_path() / "vapor_dxil_cache";
    std::error_code ec;
    std::filesystem::create_directories(shaderCacheDir, ec);

    // --- Timestamp queries -------------------------------------------------
    {
        D3D12_QUERY_HEAP_DESC qd{};
        qd.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
        qd.Count = TIMESTAMP_QUERY_CAPACITY * MAX_FRAMES_IN_FLIGHT;
        throwIfFailed(device->CreateQueryHeap(&qd, IID_PPV_ARGS(timestampHeap.put())), "CreateQueryHeap");

        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = sizeof(Uint64) * TIMESTAMP_QUERY_CAPACITY * MAX_FRAMES_IN_FLIGHT;
        rd.Height = 1;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        throwIfFailed(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                                      D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                      IID_PPV_ARGS(timestampReadback.put())),
                      "timestamp readback");
    }

    // --- Swapchain ---------------------------------------------------------
    ensureSwapchain();

    fmt::print("RHI_D3D12 initialized: SM {}.{}, RT={}, mesh={}, MDI={}, bindless={}\n",
               (int)highestShaderModel >> 4, (int)highestShaderModel & 0xF,
               capabilities.raytracing, capabilities.meshShaders,
               capabilities.multiDrawIndirect, capabilities.bindlessTextures);
    return true;
}

void RHI_D3D12::ensureSwapchain() {
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(window, &w, &h);
    w = std::max(w, 1);
    h = std::max(h, 1);
    if (swapchain && (Uint32)w == swapchainWidth && (Uint32)h == swapchainHeight) return;

    waitIdle();
    for (Uint32 i = 0; i < SWAPCHAIN_BUFFERS; i++) {
        if (backbufferRtv[i] != UINT32_MAX) {
            cpuRtvHeap.free(backbufferRtv[i]);
            backbufferRtv[i] = UINT32_MAX;
        }
        backbuffers[i].reset();
    }

    if (!swapchain) {
        DXGI_SWAP_CHAIN_DESC1 sd{};
        sd.Width = w;
        sd.Height = h;
        // Flip-model swapchains reject sRGB formats; the RTVs reinterpret as
        // sRGB instead, which is what getSwapchainFormat() reports.
        sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        sd.SampleDesc.Count = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.BufferCount = SWAPCHAIN_BUFFERS;
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        DxPtr<IDXGISwapChain1> sc1;
        throwIfFailed(factory->CreateSwapChainForHwnd(queue, hwnd, &sd, nullptr, nullptr, sc1.put()),
                      "CreateSwapChainForHwnd");
        throwIfFailed(sc1->QueryInterface(IID_PPV_ARGS(swapchain.put())), "IDXGISwapChain3");
        factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
    } else {
        throwIfFailed(swapchain->ResizeBuffers(SWAPCHAIN_BUFFERS, w, h,
                                               DXGI_FORMAT_B8G8R8A8_UNORM, 0),
                      "ResizeBuffers");
    }
    swapchainWidth = w;
    swapchainHeight = h;

    for (Uint32 i = 0; i < SWAPCHAIN_BUFFERS; i++) {
        throwIfFailed(swapchain->GetBuffer(i, IID_PPV_ARGS(backbuffers[i].put())), "GetBuffer");
        backbufferRtv[i] = cpuRtvHeap.alloc();
        D3D12_RENDER_TARGET_VIEW_DESC rtv{};
        rtv.Format = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        device->CreateRenderTargetView(backbuffers[i], &rtv, cpuRtvHeap.cpuAt(backbufferRtv[i]));
        backbufferState[i] = D3D12_RESOURCE_STATE_PRESENT;
    }
    backbufferIndex = swapchain->GetCurrentBackBufferIndex();
}

RHI_D3D12::~RHI_D3D12() {
    // Renderer::shutdown() destroys renderer-owned resources but never calls
    // rhi->shutdown(). Every one of those destroys lands in `zombies` — raw
    // owned pointers the default destructor would not Release — and the same
    // goes for commandSigCache and mipgenPsoCache. Each leaked child holds a
    // device reference, keeping the device alive to process exit (the
    // several-hundred-object live dump from the debug layer). shutdown() is
    // idempotent (null/empty guards throughout), so an earlier explicit call
    // stays harmless.
    shutdown();
}

void RHI_D3D12::shutdown() {
    waitIdle();
    if (imguiInitialized) {
        // renderer.cpp shuts ImGui down through imguiD3D12Shutdown() first;
        // this is only the backstop.
        imguiInitialized = false;
    }
    for (auto& z : zombies) z.resource->Release();
    zombies.clear();
    for (auto& [key, sig] : commandSigCache) sig->Release();
    commandSigCache.clear();
    for (auto& [key, pso] : mipgenPsoCache) { (void)key; pso->Release(); }
    mipgenPsoCache.clear();
    buffers.clear();
    textures.clear();
    shaders.clear();
    samplers.clear();
    pipelines.clear();
    computePipelines.clear();
    accelStructs.clear();
    argumentTables.clear();
    if (fenceEvent) {
        CloseHandle(fenceEvent);
        fenceEvent = nullptr;
    }
    if (dxcompilerModule) {
        dxcUtils.reset();
        dxcCompiler.reset();
        FreeLibrary(dxcompilerModule);
        dxcompilerModule = nullptr;
    }
}

void RHI_D3D12::waitIdle() {
    if (!queue || !fence) return;
    const Uint64 v = nextFenceValue++;
    queue->Signal(fence, v);
    waitForFenceValue(v);
    retireZombies(fence->GetCompletedValue());
}

void RHI_D3D12::waitForFenceValue(Uint64 value) {
    if (fence->GetCompletedValue() < value) {
        fence->SetEventOnCompletion(value, fenceEvent);
        WaitForSingleObject(fenceEvent, INFINITE);
    }
}

void RHI_D3D12::retireZombies(Uint64 completed) {
    auto it = std::remove_if(zombies.begin(), zombies.end(), [&](Zombie& z) {
        if (z.fenceValue <= completed) {
            z.resource->Release();
            return true;
        }
        return false;
    });
    zombies.erase(it, zombies.end());
}

// ============================================================================
// Resource creation
// ============================================================================

BufferHandle RHI_D3D12::createBuffer(const BufferDesc& desc) {
    BufferResource buf;
    buf.size = desc.size;
    buf.allocSize = alignUp(std::max<size_t>(desc.size, 4), 4);
    buf.usage = desc.usage;
    buf.memoryUsage = desc.memoryUsage;

    D3D12_HEAP_PROPERTIES hp{};
    D3D12_RESOURCE_STATES initialState = D3D12_RESOURCE_STATE_COMMON;
    switch (desc.memoryUsage) {
        case MemoryUsage::CPU:
        case MemoryUsage::CPUtoGPU:
            hp.Type = D3D12_HEAP_TYPE_UPLOAD;
            initialState = D3D12_RESOURCE_STATE_GENERIC_READ;
            break;
        case MemoryUsage::GPUreadback:
            hp.Type = D3D12_HEAP_TYPE_READBACK;
            initialState = D3D12_RESOURCE_STATE_COPY_DEST;
            break;
        default:
            hp.Type = D3D12_HEAP_TYPE_DEFAULT;
            break;
    }

    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = buf.allocSize;
    rd.Height = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    // Storage/Indirect buffers get compute-written through raw UAVs.
    if (hp.Type == D3D12_HEAP_TYPE_DEFAULT &&
        (desc.usage == BufferUsage::Storage || desc.usage == BufferUsage::Indirect ||
         desc.usage == BufferUsage::Vertex || desc.usage == BufferUsage::Index)) {
        rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    }

    if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, initialState, nullptr,
                                               IID_PPV_ARGS(buf.resource.put())))) {
        fmt::print(stderr, "RHI_D3D12: createBuffer({} bytes) failed\n", desc.size);
        return {};
    }
    buf.va = buf.resource->GetGPUVirtualAddress();
    if (hp.Type != D3D12_HEAP_TYPE_DEFAULT) {
        D3D12_RANGE noRead{0, 0};
        buf.resource->Map(0, hp.Type == D3D12_HEAP_TYPE_READBACK ? nullptr : &noRead, &buf.mapped);
    }
    createBufferViews(buf);

    Uint32 id = nextResourceId++;
    buffers.emplace(id, std::move(buf));
    return BufferHandle{id};
}

void RHI_D3D12::createBufferViews(BufferResource& buf) {
    // Raw (ByteAddressBuffer) views — what spirv-cross emits for std430 SSBOs.
    buf.srvIndex = cpuSrvHeap.alloc();
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = DXGI_FORMAT_R32_TYPELESS;
    srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Buffer.NumElements = static_cast<UINT>(buf.allocSize / 4);
    srv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
    device->CreateShaderResourceView(buf.resource, &srv, cpuSrvHeap.cpuAt(buf.srvIndex));

    buf.uavIndex = cpuSrvHeap.alloc();
    if (resourceDesc(buf.resource).Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) {
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.Format = DXGI_FORMAT_R32_TYPELESS;
        uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        uav.Buffer.NumElements = static_cast<UINT>(buf.allocSize / 4);
        uav.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
        device->CreateUnorderedAccessView(buf.resource, nullptr, &uav, cpuSrvHeap.cpuAt(buf.uavIndex));
    } else {
        // Upload-heap buffers can't be UAVs; a null descriptor keeps the slot
        // valid (reads return zero, writes are dropped).
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.Format = DXGI_FORMAT_R32_TYPELESS;
        uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        uav.Buffer.NumElements = 1;
        uav.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
        device->CreateUnorderedAccessView(nullptr, nullptr, &uav, cpuSrvHeap.cpuAt(buf.uavIndex));
    }
}

void RHI_D3D12::destroyBuffer(BufferHandle handle) {
    auto it = buffers.find(handle.id);
    if (it == buffers.end()) return;
    BufferResource& buf = it->second;
    cpuSrvHeap.free(buf.srvIndex);
    cpuSrvHeap.free(buf.uavIndex);
    if (buf.resource) {
        if (buf.mapped) buf.resource->Unmap(0, nullptr);
        deferRelease(buf.resource.p);
        buf.resource.p = nullptr;  // ownership moved to the zombie list
    }
    buffers.erase(it);
}

TextureHandle RHI_D3D12::createTexture(const TextureDesc& desc) {
    TextureResource tex;
    tex.desc = desc;
    tex.resourceFormat = convertResourceFormat(desc.format);

    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = desc.width;
    rd.Height = desc.height;
    rd.DepthOrArraySize = static_cast<UINT16>(std::max(1u, desc.arrayLayers));
    rd.MipLevels = static_cast<UINT16>(std::max(1u, desc.mipLevels));
    rd.Format = tex.resourceFormat;
    rd.SampleDesc.Count = std::max(1u, desc.sampleCount);
    rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

    const bool depth = isDepthFormat(desc.format);
    D3D12_CLEAR_VALUE clear{};
    D3D12_CLEAR_VALUE* clearPtr = nullptr;
    if (depth) {
        rd.Flags |= D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        clear.Format = depthDsvFormat(desc.format);
        clear.DepthStencil.Depth = 1.0f;
        clearPtr = &clear;
    }
    if (hasUsage(desc.usage, TextureUsage::RenderTarget) && !depth) {
        rd.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        clear.Format = tex.resourceFormat;
        clearPtr = &clear;
    }
    if (hasUsage(desc.usage, TextureUsage::Storage)) {
        rd.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    }
    // generateMipmaps() downsamples through render-target blits, so any mipped
    // color texture needs RTV capability even when created Sampled-only.
    if (!depth && desc.mipLevels > 1) {
        rd.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        clear.Format = tex.resourceFormat;
        clearPtr = &clear;
    }

    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    tex.state = D3D12_RESOURCE_STATE_COMMON;
    if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, tex.state,
                                               (rd.Flags & (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET |
                                                            D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL))
                                                   ? clearPtr : nullptr,
                                               IID_PPV_ARGS(tex.resource.put())))) {
        fmt::print(stderr, "RHI_D3D12: createTexture {}x{} failed\n", desc.width, desc.height);
        return {};
    }

    // Whole-texture SRV
    if (desc.sampleCount <= 1) {
        tex.srvIndex = cpuSrvHeap.alloc();
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = depth ? depthSrvFormat(desc.format) : tex.resourceFormat;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        if (desc.isCube) {
            srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
            srv.TextureCube.MipLevels = rd.MipLevels;
        } else if (desc.arrayLayers > 1) {
            srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
            srv.Texture2DArray.MipLevels = rd.MipLevels;
            srv.Texture2DArray.ArraySize = desc.arrayLayers;
        } else {
            srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srv.Texture2D.MipLevels = rd.MipLevels;
        }
        device->CreateShaderResourceView(tex.resource, &srv, cpuSrvHeap.cpuAt(tex.srvIndex));
    }

    // Mip-0 UAV for storage binds
    if (hasUsage(desc.usage, TextureUsage::Storage) && desc.sampleCount <= 1) {
        tex.uavIndex = cpuSrvHeap.alloc();
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.Format = tex.resourceFormat;
        if (desc.arrayLayers > 1) {
            uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
            uav.Texture2DArray.ArraySize = desc.arrayLayers;
        } else {
            uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        }
        device->CreateUnorderedAccessView(tex.resource, nullptr, &uav, cpuSrvHeap.cpuAt(tex.uavIndex));
    }

    Uint32 id = nextResourceId++;
    textures.emplace(id, std::move(tex));
    return TextureHandle{id};
}

TextureHandle RHI_D3D12::createTextureView(const TextureViewDesc& desc) {
    auto srcIt = textures.find(desc.source.id);
    if (srcIt == textures.end()) return {};
    TextureResource& src = srcIt->second;
    if (desc.baseArrayLayer + desc.layerCount > std::max(1u, src.desc.arrayLayers)) return {};

    TextureResource view;
    view.isView = true;
    view.viewSourceId = desc.source.id;
    view.desc = src.desc;
    view.desc.arrayLayers = desc.layerCount;
    view.desc.isCube = false;
    view.resourceFormat = src.resourceFormat;

    view.srvIndex = cpuSrvHeap.alloc();
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = isDepthFormat(src.desc.format) ? depthSrvFormat(src.desc.format) : src.resourceFormat;
    if (desc.swizzle == TextureSwizzle::RRR1) {
        srv.Shader4ComponentMapping = D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(
            D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_0,
            D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_0,
            D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_0,
            D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_1);
    } else {
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    }
    if (desc.layerCount > 1 || src.desc.arrayLayers > 1) {
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
        srv.Texture2DArray.MipLevels = std::max(1u, src.desc.mipLevels);
        srv.Texture2DArray.FirstArraySlice = desc.baseArrayLayer;
        srv.Texture2DArray.ArraySize = desc.layerCount;
    } else {
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Texture2D.MipLevels = std::max(1u, src.desc.mipLevels);
    }
    device->CreateShaderResourceView(src.resource, &srv, cpuSrvHeap.cpuAt(view.srvIndex));

    Uint32 id = nextResourceId++;
    textures.emplace(id, std::move(view));
    return TextureHandle{id};
}

void RHI_D3D12::destroyTexture(TextureHandle handle) {
    auto it = textures.find(handle.id);
    if (it == textures.end()) return;
    TextureResource& tex = it->second;
    cpuSrvHeap.free(tex.srvIndex);
    cpuSrvHeap.free(tex.uavIndex);
    for (auto& [k, v] : tex.rtvCache) cpuRtvHeap.free(v);
    for (auto& [k, v] : tex.dsvCache) cpuDsvHeap.free(v);
    for (auto& [k, v] : tex.mipSrvCache) cpuSrvHeap.free(v);
    if (tex.imguiSlot != UINT32_MAX) imguiFree.push_back(tex.imguiSlot);
    if (tex.resource && !tex.isView) {
        deferRelease(tex.resource.p);
        tex.resource.p = nullptr;
    }
    textures.erase(it);
}

RHI_D3D12::TextureResource* RHI_D3D12::resolveTexture(Uint32 id) {
    auto it = textures.find(id);
    if (it == textures.end()) return nullptr;
    if (!it->second.isView) return &it->second;
    auto src = textures.find(it->second.viewSourceId);
    return src != textures.end() ? &src->second : nullptr;
}

SamplerHandle RHI_D3D12::createSampler(const SamplerDesc& desc) {
    SamplerResource s;
    s.desc.Filter = convertFilter(desc);
    s.desc.AddressU = convertAddressMode(desc.addressModeU);
    s.desc.AddressV = convertAddressMode(desc.addressModeV);
    s.desc.AddressW = convertAddressMode(desc.addressModeW);
    s.desc.MipLODBias = desc.mipLodBias;
    s.desc.MaxAnisotropy = desc.enableAnisotropy ? static_cast<UINT>(std::max(1.0f, desc.maxAnisotropy)) : 1;
    s.desc.ComparisonFunc = desc.enableCompare ? convertCompareOp(desc.compareOp) : D3D12_COMPARISON_FUNC_NEVER;
    // Vulkan backend uses INT_OPAQUE_BLACK for ClampToBorder
    s.desc.BorderColor[0] = 0.0f;
    s.desc.BorderColor[1] = 0.0f;
    s.desc.BorderColor[2] = 0.0f;
    s.desc.BorderColor[3] = 1.0f;
    s.desc.MinLOD = desc.minLod;
    s.desc.MaxLOD = desc.maxLod;
    s.hash = fnv1a64(&s.desc, sizeof(s.desc));

    Uint32 id = nextResourceId++;
    samplers.emplace(id, s);
    return SamplerHandle{id};
}

void RHI_D3D12::destroySampler(SamplerHandle handle) {
    samplers.erase(handle.id);
}

// ----------------------------------------------------------------------------
// Shader compilation: SPIR-V → (spirv-cross) HLSL → (dxc) DXIL, disk-cached.
// ----------------------------------------------------------------------------

std::vector<Uint8> RHI_D3D12::compileSpirvToDxil(const void* spirv, size_t size, ShaderStage stage) {
    // Per-stage shader model: SM 6.8 gives native SV_StartInstanceLocation
    // (gl_InstanceIndex under MDI); fall back to 6.6 + the spirv-cross
    // base-instance compensation cbuffer on older drivers (capabilities
    // already gated MDI off there). Mesh/task floor is 6.5.
    const bool sm68 = highestShaderModel >= D3D_SHADER_MODEL_6_8;
    Uint32 scModel = sm68 ? 68 : 66;
    const wchar_t* profile = L"cs_6_6";
    switch (stage) {
        case ShaderStage::Vertex:   profile = sm68 ? L"vs_6_8" : L"vs_6_6"; break;
        case ShaderStage::Fragment: profile = sm68 ? L"ps_6_8" : L"ps_6_6"; break;
        // cs_6_6 unconditionally: it clears the SM 6.5 floor that inline ray
        // queries (RayQuery<>) need, so the RT kernels and the plain compute
        // kernels share one profile. (This was a ternary on
        // capabilities.raytracing with cs_6_6 on both arms.)
        case ShaderStage::Compute:  profile = L"cs_6_6"; break;
        case ShaderStage::Task:     profile = L"as_6_5"; scModel = std::max(scModel, 65u); break;
        case ShaderStage::Mesh:     profile = L"ms_6_5"; scModel = std::max(scModel, 65u); break;
    }
    if (stage == ShaderStage::Task || stage == ShaderStage::Mesh) scModel = 65;

    // Disk cache: keyed by SPIR-V bytes + stage + shader model.
    Uint64 key = fnv1a64(spirv, size);
    key = fnv1a64(&stage, sizeof(stage), key);
    key = fnv1a64(&scModel, sizeof(scModel), key);
    std::filesystem::path cachePath = shaderCacheDir / fmt::format("{:016x}.dxil", key);
    {
        std::ifstream in(cachePath, std::ios::binary);
        if (in) {
            std::vector<Uint8> dxil((std::istreambuf_iterator<char>(in)), {});
            if (!dxil.empty()) return dxil;
        }
    }

    // --- SPIR-V → HLSL ---
    std::vector<uint32_t> words(size / 4);
    std::memcpy(words.data(), spirv, words.size() * 4);
    std::string hlsl;
    try {
        spirv_cross::CompilerHLSL compiler(std::move(words));
        spirv_cross::CompilerHLSL::Options hlslOpts;
        hlslOpts.shader_model = scModel;
        if (!sm68 && stage == ShaderStage::Vertex) {
            hlslOpts.support_nonzero_base_vertex_base_instance = true;
        }
        compiler.set_hlsl_options(hlslOpts);
        if (!sm68 && stage == ShaderStage::Vertex) {
            // Compensation cbuffer at (b1, space15) — the kGfxRootBaseVertex
            // root constants.
            compiler.set_hlsl_aux_buffer_binding(spirv_cross::HLSL_AUX_BINDING_BASE_VERTEX_INSTANCE, 1, 15);
        }

        // Push constants → the root-constant register the root signatures declare.
        for (auto model : {spv::ExecutionModelVertex, spv::ExecutionModelFragment,
                           spv::ExecutionModelGLCompute, spv::ExecutionModelTaskEXT,
                           spv::ExecutionModelMeshEXT}) {
            spirv_cross::HLSLResourceBinding pc;
            pc.stage = model;
            pc.desc_set = spirv_cross::ResourceBindingPushConstantDescriptorSet;
            pc.binding = spirv_cross::ResourceBindingPushConstantBinding;
            pc.cbv.register_binding = 0;
            pc.cbv.register_space = 15;
            compiler.add_hlsl_resource_binding(pc);
        }
        hlsl = compiler.compile();
    } catch (const std::exception& e) {
        throw std::runtime_error(fmt::format("RHI_D3D12: spirv-cross failed: {}", e.what()));
    }

    // VAPOR_DUMP_HLSL=1 writes the cross-compiled HLSL beside its DXIL cache
    // entry. Register/semantic assignment is what stage-linkage bugs turn on,
    // and it is otherwise invisible — the backend only ever keeps the DXIL.
    if (const char* d = std::getenv("VAPOR_DUMP_HLSL"); d && d[0] == '1') {
        std::ofstream h(shaderCacheDir / fmt::format("{:016x}.hlsl", key));
        if (h) h << hlsl;
    }

    // --- HLSL → DXIL ---
    auto runDxc = [&](bool disableOptimizations) -> std::vector<Uint8> {
        DxcBuffer src{};
        src.Ptr = hlsl.data();
        src.Size = hlsl.size();
        src.Encoding = DXC_CP_UTF8;
        std::vector<LPCWSTR> args = {L"-E", L"main", L"-T", profile};
        if (disableOptimizations) {
            args.push_back(L"-Od");
        } else {
            args.push_back(L"-O3");
        }
        args.push_back(L"-Qstrip_reflect");
        DxPtr<IDxcResult> result;
        if (FAILED(dxcCompiler->Compile(&src, args.data(), static_cast<UINT32>(args.size()), nullptr,
                                        kIID_IDxcResult, result.putVoid()))) {
            return {};
        }
        HRESULT status = S_OK;
        result->GetStatus(&status);
        if (FAILED(status)) {
            DxPtr<IDxcBlobUtf8> errors;
            result->GetOutput(DXC_OUT_ERRORS, kIID_IDxcBlobUtf8, errors.putVoid(), nullptr);
            if (!disableOptimizations) {
                return {};  // caller retries at -Od
            }
            throw std::runtime_error(fmt::format(
                "RHI_D3D12: dxc failed: {}",
                errors && errors->GetStringLength() ? errors->GetStringPointer() : "?"));
        }
        DxPtr<IDxcBlob> object;
        result->GetOutput(DXC_OUT_OBJECT, kIID_IDxcBlob, object.putVoid(), nullptr);
        if (!object) return {};
        auto* p = static_cast<const Uint8*>(object->GetBufferPointer());
        return std::vector<Uint8>(p, p + object->GetBufferSize());
    };

    std::vector<Uint8> dxil = runDxc(false);
    if (dxil.empty()) {
        // The DXIL validator's mesh-stage rules (e.g. single dominating
        // SetMeshOutputCounts) can reject optimizer-restructured control flow;
        // unoptimized DXIL still JITs fine in the driver.
        dxil = runDxc(true);
    }

    std::ofstream out(cachePath, std::ios::binary);
    if (out) out.write(reinterpret_cast<const char*>(dxil.data()), dxil.size());
    return dxil;
}

ShaderHandle RHI_D3D12::createShader(const ShaderDesc& desc) {
    if (!desc.code || desc.codeSize < 8) return {};
    // The renderer hands this backend the same .spv files as Vulkan.
    const uint32_t magic = *static_cast<const uint32_t*>(desc.code);
    if (magic != 0x07230203u) {
        fmt::print(stderr, "RHI_D3D12: createShader expects SPIR-V input\n");
        return {};
    }
    ShaderResource shader;
    shader.stage = desc.stage;
    try {
        shader.dxil = compileSpirvToDxil(desc.code, desc.codeSize, desc.stage);
    } catch (const std::exception& e) {
        fmt::print(stderr, "{}\n", e.what());
        return {};
    }
    if (shader.dxil.empty()) return {};
    Uint32 id = nextResourceId++;
    shaders.emplace(id, std::move(shader));
    return ShaderHandle{id};
}

void RHI_D3D12::destroyShader(ShaderHandle handle) {
    shaders.erase(handle.id);
}

// ============================================================================
// Pipelines
// ============================================================================

PipelineHandle RHI_D3D12::createPipeline(const PipelineDesc& desc) {
    auto vsIt = shaders.find(desc.vertexShader.id);
    auto fsIt = shaders.find(desc.fragmentShader.id);
    if (vsIt == shaders.end() || fsIt == shaders.end()) return {};

    // Input layout: spirv-cross names vertex inputs TEXCOORD<location>.
    std::vector<D3D12_INPUT_ELEMENT_DESC> elements;
    elements.reserve(desc.vertexLayout.attributes.size());
    for (const auto& attr : desc.vertexLayout.attributes) {
        D3D12_INPUT_ELEMENT_DESC e{};
        e.SemanticName = "TEXCOORD";
        e.SemanticIndex = attr.location;
        e.Format = convertAttributeFormat(attr.format);
        e.InputSlot = 0;
        e.AlignedByteOffset = attr.offset;
        e.InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
        elements.push_back(e);
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = graphicsRootSig;
    pd.VS = {vsIt->second.dxil.data(), vsIt->second.dxil.size()};
    pd.PS = {fsIt->second.dxil.data(), fsIt->second.dxil.size()};
    pd.InputLayout = {elements.data(), static_cast<UINT>(elements.size())};
    pd.PrimitiveTopologyType = convertTopologyType(desc.topology);
    pd.SampleMask = UINT_MAX;
    pd.SampleDesc.Count = std::max(1u, desc.sampleCount);

    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = desc.cullMode == CullMode::None  ? D3D12_CULL_MODE_NONE
                                 : desc.cullMode == CullMode::Front ? D3D12_CULL_MODE_FRONT
                                                                    : D3D12_CULL_MODE_BACK;
    // Vulkan renders through a negative-height viewport (Y-up NDC), matching
    // D3D12's native convention — winding maps 1:1.
    pd.RasterizerState.FrontCounterClockwise = desc.frontFaceCounterClockwise ? TRUE : FALSE;
    pd.RasterizerState.DepthClipEnable = TRUE;

    D3D12_RENDER_TARGET_BLEND_DESC blend = convertBlendMode(desc.blendMode);
    pd.NumRenderTargets = static_cast<UINT>(std::max<size_t>(1, desc.colorAttachmentFormats.size()));
    for (UINT i = 0; i < pd.NumRenderTargets && i < 8; i++) {
        PixelFormat f = i < desc.colorAttachmentFormats.size() ? desc.colorAttachmentFormats[i]
                                                              : PixelFormat::Swapchain;
        pd.RTVFormats[i] = f == PixelFormat::Swapchain ? DXGI_FORMAT_B8G8R8A8_UNORM_SRGB
                                                       : convertResourceFormat(f);
        pd.BlendState.RenderTarget[i] = blend;
    }
    pd.BlendState.IndependentBlendEnable = FALSE;

    if (desc.hasDepthAttachment) {
        pd.DSVFormat = depthDsvFormat(desc.depthAttachmentFormat);
        pd.DepthStencilState.DepthEnable = desc.depthTest ? TRUE : FALSE;
        pd.DepthStencilState.DepthWriteMask = (desc.depthTest && desc.depthWrite)
                                                  ? D3D12_DEPTH_WRITE_MASK_ALL
                                                  : D3D12_DEPTH_WRITE_MASK_ZERO;
        pd.DepthStencilState.DepthFunc = convertCompareOp(desc.depthCompareOp);
    }

    PipelineResource pipe;
    if (FAILED(device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(pipe.pso.put())))) {
        fmt::print(stderr, "RHI_D3D12: CreateGraphicsPipelineState failed\n");
        return {};
    }
    pipe.topology = convertTopology(desc.topology);
    pipe.vertexStride = desc.vertexLayout.stride;

    Uint32 id = nextResourceId++;
    pipelines.emplace(id, std::move(pipe));
    return PipelineHandle{id};
}

PipelineHandle RHI_D3D12::createMeshPipeline(const MeshPipelineDesc& desc) {
    if (!capabilities.meshShaders) return {};
    auto msIt = shaders.find(desc.meshShader.id);
    auto fsIt = shaders.find(desc.fragmentShader.id);
    if (msIt == shaders.end() || fsIt == shaders.end()) return {};
    auto tsIt = shaders.find(desc.taskShader.id);  // optional

    // Mesh PSOs go through the subobject-stream API (d3dx12-style helpers are
    // hand-rolled here to stay header-light).
    #define VAPOR_STREAM_FIELD(name, subobjType, valueType)                        \
        struct alignas(void*) name##_t {                                           \
            D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type = subobjType;                 \
            valueType value{};                                                     \
        } name

    struct Stream {
        VAPOR_STREAM_FIELD(rootSig, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE, ID3D12RootSignature*);
        VAPOR_STREAM_FIELD(as, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS, D3D12_SHADER_BYTECODE);
        VAPOR_STREAM_FIELD(ms, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS, D3D12_SHADER_BYTECODE);
        VAPOR_STREAM_FIELD(ps, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS, D3D12_SHADER_BYTECODE);
        VAPOR_STREAM_FIELD(blend, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND, D3D12_BLEND_DESC);
        VAPOR_STREAM_FIELD(sampleMask, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK, UINT);
        VAPOR_STREAM_FIELD(raster, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER, D3D12_RASTERIZER_DESC);
        VAPOR_STREAM_FIELD(depth, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL, D3D12_DEPTH_STENCIL_DESC);
        VAPOR_STREAM_FIELD(rtvs, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS, D3D12_RT_FORMAT_ARRAY);
        VAPOR_STREAM_FIELD(dsvFormat, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT, DXGI_FORMAT);
        VAPOR_STREAM_FIELD(sampleDesc, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC, DXGI_SAMPLE_DESC);
    } stream;
    #undef VAPOR_STREAM_FIELD

    stream.rootSig.value = graphicsRootSig;
    if (tsIt != shaders.end()) {
        stream.as.value = {tsIt->second.dxil.data(), tsIt->second.dxil.size()};
    }
    stream.ms.value = {msIt->second.dxil.data(), msIt->second.dxil.size()};
    stream.ps.value = {fsIt->second.dxil.data(), fsIt->second.dxil.size()};

    D3D12_RENDER_TARGET_BLEND_DESC blend = convertBlendMode(desc.blendMode);
    UINT numRT = static_cast<UINT>(std::max<size_t>(1, desc.colorAttachmentFormats.size()));
    stream.rtvs.value.NumRenderTargets = numRT;
    for (UINT i = 0; i < numRT && i < 8; i++) {
        PixelFormat f = i < desc.colorAttachmentFormats.size() ? desc.colorAttachmentFormats[i]
                                                               : PixelFormat::Swapchain;
        stream.rtvs.value.RTFormats[i] = f == PixelFormat::Swapchain ? DXGI_FORMAT_B8G8R8A8_UNORM_SRGB
                                                                     : convertResourceFormat(f);
        stream.blend.value.RenderTarget[i] = blend;
    }
    stream.sampleMask.value = UINT_MAX;
    stream.raster.value.FillMode = D3D12_FILL_MODE_SOLID;
    stream.raster.value.CullMode = desc.cullMode == CullMode::None  ? D3D12_CULL_MODE_NONE
                                  : desc.cullMode == CullMode::Front ? D3D12_CULL_MODE_FRONT
                                                                     : D3D12_CULL_MODE_BACK;
    stream.raster.value.FrontCounterClockwise = desc.frontFaceCounterClockwise ? TRUE : FALSE;
    stream.raster.value.DepthClipEnable = TRUE;
    if (desc.hasDepthAttachment) {
        stream.depth.value.DepthEnable = desc.depthTest ? TRUE : FALSE;
        stream.depth.value.DepthWriteMask = (desc.depthTest && desc.depthWrite)
                                                ? D3D12_DEPTH_WRITE_MASK_ALL
                                                : D3D12_DEPTH_WRITE_MASK_ZERO;
        stream.depth.value.DepthFunc = convertCompareOp(desc.depthCompareOp);
        stream.dsvFormat.value = depthDsvFormat(desc.depthAttachmentFormat);
    } else {
        stream.depth.value.DepthEnable = FALSE;
        stream.dsvFormat.value = DXGI_FORMAT_UNKNOWN;
    }
    stream.sampleDesc.value = {std::max(1u, desc.sampleCount), 0};

    DxPtr<ID3D12Device2> device2;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(device2.put())))) return {};
    D3D12_PIPELINE_STATE_STREAM_DESC sd{};
    sd.SizeInBytes = sizeof(stream);
    sd.pPipelineStateSubobjectStream = &stream;

    PipelineResource pipe;
    if (FAILED(device2->CreatePipelineState(&sd, IID_PPV_ARGS(pipe.pso.put())))) {
        fmt::print(stderr, "RHI_D3D12: mesh CreatePipelineState failed\n");
        return {};
    }
    pipe.isMesh = true;

    Uint32 id = nextResourceId++;
    pipelines.emplace(id, std::move(pipe));
    return PipelineHandle{id};
}

void RHI_D3D12::destroyPipeline(PipelineHandle handle) {
    pipelines.erase(handle.id);
}

ComputePipelineHandle RHI_D3D12::createComputePipeline(const ComputePipelineDesc& desc) {
    auto csIt = shaders.find(desc.computeShader.id);
    if (csIt == shaders.end()) return {};
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = computeRootSig;
    pd.CS = {csIt->second.dxil.data(), csIt->second.dxil.size()};
    ComputePipelineResource pipe;
    if (FAILED(device->CreateComputePipelineState(&pd, IID_PPV_ARGS(pipe.pso.put())))) {
        fmt::print(stderr, "RHI_D3D12: CreateComputePipelineState failed\n");
        return {};
    }
    Uint32 id = nextResourceId++;
    computePipelines.emplace(id, std::move(pipe));
    return ComputePipelineHandle{id};
}

void RHI_D3D12::destroyComputePipeline(ComputePipelineHandle handle) {
    computePipelines.erase(handle.id);
}

// ============================================================================
// Acceleration structures (DXR). Semantics mirror the Metal backend: BLAS with
// all geometries in one structure (float3 positions at offset 0, uint32
// indices), TLAS fully rebuilt on update with rotated slots so no in-flight
// frame reads a structure being rewritten.
// ============================================================================

void RHI_D3D12::ensureAccelBuffer(AccelBuffer& buf, size_t size, bool asResult, bool mappable) {
    if (buf.resource && buf.size >= size) return;
    if (buf.resource) {
        if (buf.mapped) buf.resource->Unmap(0, nullptr);
        deferRelease(buf.resource.p);
        buf.resource.p = nullptr;
        buf.mapped = nullptr;
    }
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = mappable ? D3D12_HEAP_TYPE_UPLOAD : D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = alignUp(std::max<size_t>(size, 256), 256);
    rd.Height = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (!mappable) rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    D3D12_RESOURCE_STATES state = mappable ? D3D12_RESOURCE_STATE_GENERIC_READ
                                : asResult ? D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE
                                           : D3D12_RESOURCE_STATE_COMMON;
    throwIfFailed(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, state, nullptr,
                                                  IID_PPV_ARGS(buf.resource.put())),
                  "accel buffer");
    buf.size = rd.Width;
    if (mappable) {
        D3D12_RANGE noRead{0, 0};
        buf.resource->Map(0, &noRead, &buf.mapped);
    }
}

AccelStructHandle RHI_D3D12::createAccelerationStructure(const AccelStructDesc& desc) {
    if (!capabilities.raytracing) return {};
    AccelStructResource as;
    as.desc = desc;
    Uint32 id = nextResourceId++;
    accelStructs.emplace(id, std::move(as));
    return AccelStructHandle{id};
}

void RHI_D3D12::destroyAccelerationStructure(AccelStructHandle handle) {
    auto it = accelStructs.find(handle.id);
    if (it == accelStructs.end()) return;
    for (Uint32 s = 0; s < AccelStructResource::kTlasSlots; s++) {
        for (AccelBuffer* b : {&it->second.result[s], &it->second.scratch[s], &it->second.instances[s]}) {
            if (b->resource) {
                if (b->mapped) b->resource->Unmap(0, nullptr);
                deferRelease(b->resource.p);
                b->resource.p = nullptr;
            }
        }
    }
    accelStructs.erase(it);
}

void RHI_D3D12::buildAccelStructInternal(AccelStructResource& as, ID3D12GraphicsCommandList4* list, Uint32 slot) {
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs{};
    inputs.Flags = as.desc.preferFastBuild ? D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD
                                           : D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;

    std::vector<D3D12_RAYTRACING_GEOMETRY_DESC> geoDescs;
    if (as.desc.type == AccelStructType::BottomLevel) {
        inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
        geoDescs.reserve(as.desc.geometries.size());
        for (const auto& g : as.desc.geometries) {
            auto vbIt = buffers.find(g.vertexBuffer.id);
            auto ibIt = buffers.find(g.indexBuffer.id);
            if (vbIt == buffers.end() || ibIt == buffers.end()) continue;
            D3D12_RAYTRACING_GEOMETRY_DESC gd{};
            gd.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
            gd.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
            gd.Triangles.VertexBuffer.StartAddress = vbIt->second.va;
            gd.Triangles.VertexBuffer.StrideInBytes = g.vertexStride;
            gd.Triangles.VertexCount = g.vertexCount;
            gd.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;  // float3 @ offset 0 (Metal parity)
            gd.Triangles.IndexBuffer = ibIt->second.va;
            gd.Triangles.IndexCount = g.indexCount;
            gd.Triangles.IndexFormat = DXGI_FORMAT_R32_UINT;
            geoDescs.push_back(gd);
        }
        inputs.NumDescs = static_cast<UINT>(geoDescs.size());
        inputs.pGeometryDescs = geoDescs.data();
    } else {
        inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
        // Instance descs were already written into instances[slot] by the caller.
        inputs.NumDescs = static_cast<UINT>(as.desc.instances.size());
        inputs.InstanceDescs = as.instances[slot].resource ? as.instances[slot].resource->GetGPUVirtualAddress() : 0;
    }

    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO prebuild{};
    device5->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &prebuild);
    ensureAccelBuffer(as.result[slot], prebuild.ResultDataMaxSizeInBytes, true, false);
    ensureAccelBuffer(as.scratch[slot], prebuild.ScratchDataSizeInBytes, false, false);

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build{};
    build.Inputs = inputs;
    build.DestAccelerationStructureData = as.result[slot].resource->GetGPUVirtualAddress();
    build.ScratchAccelerationStructureData = as.scratch[slot].resource->GetGPUVirtualAddress();
    list->BuildRaytracingAccelerationStructure(&build, 0, nullptr);

    // Builds reading this structure (TLAS over BLASes) and ray queries both
    // need the write flushed.
    D3D12_RESOURCE_BARRIER uav{};
    uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uav.UAV.pResource = as.result[slot].resource;
    list->ResourceBarrier(1, &uav);
    as.built = true;
}

void RHI_D3D12::buildAccelerationStructure(AccelStructHandle handle) {
    auto it = accelStructs.find(handle.id);
    if (it == accelStructs.end() || !capabilities.raytracing) return;
    AccelStructResource& as = it->second;

    if (as.desc.type == AccelStructType::TopLevel) {
        updateAccelerationStructure(handle, as.desc.instances);
        return;
    }
    // BLAS geometry lives in engine buffers whose uploads may still be queued —
    // Metal parity: block until they land, then build synchronously (load path).
    submitUploads(true);
    executeImmediate([&](ID3D12GraphicsCommandList* raw) {
        DxPtr<ID3D12GraphicsCommandList4> list4;
        raw->QueryInterface(IID_PPV_ARGS(list4.put()));
        if (list4) buildAccelStructInternal(as, list4, 0);
    });
}

void RHI_D3D12::updateAccelerationStructure(AccelStructHandle handle, const std::vector<AccelStructInstance>& instances) {
    auto it = accelStructs.find(handle.id);
    if (it == accelStructs.end() || !capabilities.raytracing) return;
    AccelStructResource& as = it->second;
    if (as.desc.type != AccelStructType::TopLevel) return;

    as.desc.instances = instances;
    const Uint32 slot = (as.currentSlot + 1) % AccelStructResource::kTlasSlots;

    // Write instance descs (upload heap, CPU-visible)
    const size_t bytes = std::max<size_t>(1, instances.size()) * sizeof(D3D12_RAYTRACING_INSTANCE_DESC);
    ensureAccelBuffer(as.instances[slot], bytes, false, true);
    auto* descs = static_cast<D3D12_RAYTRACING_INSTANCE_DESC*>(as.instances[slot].mapped);
    for (size_t i = 0; i < instances.size(); i++) {
        const AccelStructInstance& inst = instances[i];
        auto blasIt = accelStructs.find(inst.blas.id);
        D3D12_RAYTRACING_INSTANCE_DESC d{};
        // D3D12 instance transforms are row-major 3x4 — transpose glm's columns.
        for (int r = 0; r < 3; r++) {
            for (int c = 0; c < 4; c++) {
                d.Transform[r][c] = inst.transform[c][r];
            }
        }
        d.InstanceID = inst.instanceID & 0xFFFFFFu;
        d.InstanceMask = inst.mask & 0xFFu;
        d.Flags = D3D12_RAYTRACING_INSTANCE_FLAG_FORCE_OPAQUE;
        d.AccelerationStructure = (blasIt != accelStructs.end() && blasIt->second.built)
                                      ? blasIt->second.result[0].resource->GetGPUVirtualAddress()
                                      : 0;
        descs[i] = d;
    }

    if (insideFrame && frameList4) {
        buildAccelStructInternal(as, frameList4, slot);
    } else {
        executeImmediate([&](ID3D12GraphicsCommandList* raw) {
            DxPtr<ID3D12GraphicsCommandList4> list4;
            raw->QueryInterface(IID_PPV_ARGS(list4.put()));
            if (list4) buildAccelStructInternal(as, list4, slot);
        });
    }
    as.currentSlot = slot;
}

// ============================================================================
// Command stream helpers
// ============================================================================

ID3D12GraphicsCommandList* RHI_D3D12::ensureUploadList() {
    if (!uploadListOpen) {
        uploadAllocator->Reset();
        uploadList->Reset(uploadAllocator, nullptr);
        uploadListOpen = true;
    }
    return uploadList;
}

ID3D12GraphicsCommandList* RHI_D3D12::activeList() {
    return frameListOpen ? frameList.p : ensureUploadList();
}

void RHI_D3D12::submitUploads(bool wait) {
    if (!uploadListOpen) {
        if (wait) waitIdle();
        return;
    }
    uploadList->Close();
    uploadListOpen = false;
    ID3D12CommandList* lists[] = {uploadList.p};
    queue->ExecuteCommandLists(1, lists);
    const Uint64 v = nextFenceValue++;
    queue->Signal(fence, v);
    if (wait) {
        waitForFenceValue(v);
        retireZombies(fence->GetCompletedValue());
    }
}

void RHI_D3D12::executeImmediate(const std::function<void(ID3D12GraphicsCommandList*)>& record) {
    // One-off synchronous command list (loading-path operations: AS builds,
    // out-of-frame mip generation).
    DxPtr<ID3D12CommandAllocator> alloc;
    DxPtr<ID3D12GraphicsCommandList> list;
    throwIfFailed(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(alloc.put())),
                  "immediate allocator");
    throwIfFailed(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr,
                                            IID_PPV_ARGS(list.put())),
                  "immediate list");
    record(list);
    list->Close();
    ID3D12CommandList* lists[] = {list.p};
    queue->ExecuteCommandLists(1, lists);
    const Uint64 v = nextFenceValue++;
    queue->Signal(fence, v);
    waitForFenceValue(v);
    retireZombies(fence->GetCompletedValue());
}

void RHI_D3D12::transitionTexture(TextureResource& tex, D3D12_RESOURCE_STATES newState,
                                  ID3D12GraphicsCommandList* list) {
    if (tex.state == newState) return;
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = tex.resource;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = tex.state;
    b.Transition.StateAfter = newState;
    list->ResourceBarrier(1, &b);
    tex.state = newState;
}

// ============================================================================
// Resource updates
// ============================================================================

void RHI_D3D12::updateBuffer(BufferHandle handle, const void* data, size_t offset, size_t size) {
    auto it = buffers.find(handle.id);
    if (it == buffers.end() || !data || size == 0) return;
    BufferResource& buf = it->second;

    if (buf.mapped) {
        std::memcpy(static_cast<Uint8*>(buf.mapped) + offset, data, size);
        return;
    }

    // GPU-only: stage through a transient upload buffer on the upload stream.
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = alignUp(size, 4);
    rd.Height = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    DxPtr<ID3D12Resource> staging;
    if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                               D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                               IID_PPV_ARGS(staging.put())))) {
        return;
    }
    void* mapped = nullptr;
    D3D12_RANGE noRead{0, 0};
    staging->Map(0, &noRead, &mapped);
    std::memcpy(mapped, data, size);
    staging->Unmap(0, nullptr);

    ID3D12GraphicsCommandList* list = activeList();
    list->CopyBufferRegion(it->second.resource, offset, staging, 0, size);
    deferRelease(staging.p);
    staging.p = nullptr;
}

void RHI_D3D12::updateTexture(TextureHandle handle, const void* data, size_t size,
                              Uint32 mipLevel, Uint32 arrayLayer) {
    auto it = textures.find(handle.id);
    if (it == textures.end() || !data) return;
    TextureResource& tex = it->second;
    if (tex.isView) return;

    const Uint32 mipW = std::max(1u, tex.desc.width >> mipLevel);
    const Uint32 mipH = std::max(1u, tex.desc.height >> mipLevel);
    const Uint32 bpp = pixelFormatBytesPerPixel(tex.desc.format);
    const Uint32 tightPitch = mipW * bpp;
    const Uint32 alignedPitch = static_cast<Uint32>(alignUp(tightPitch, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT));

    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = static_cast<Uint64>(alignedPitch) * mipH;
    rd.Height = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    DxPtr<ID3D12Resource> staging;
    if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                               D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                               IID_PPV_ARGS(staging.put())))) {
        return;
    }
    Uint8* mapped = nullptr;
    D3D12_RANGE noRead{0, 0};
    staging->Map(0, &noRead, reinterpret_cast<void**>(&mapped));
    const Uint8* srcRows = static_cast<const Uint8*>(data);
    const size_t copyPitch = std::min<size_t>(tightPitch, size / std::max(1u, mipH));
    for (Uint32 y = 0; y < mipH; y++) {
        std::memcpy(mapped + size_t(y) * alignedPitch, srcRows + size_t(y) * tightPitch, copyPitch);
    }
    staging->Unmap(0, nullptr);

    ID3D12GraphicsCommandList* list = activeList();
    transitionTexture(tex, D3D12_RESOURCE_STATE_COPY_DEST, list);

    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = tex.resource;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.SubresourceIndex = mipLevel + arrayLayer * std::max(1u, tex.desc.mipLevels);
    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = staging;
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint.Footprint.Format = tex.resourceFormat;
    src.PlacedFootprint.Footprint.Width = mipW;
    src.PlacedFootprint.Footprint.Height = mipH;
    src.PlacedFootprint.Footprint.Depth = 1;
    src.PlacedFootprint.Footprint.RowPitch = alignedPitch;
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

    transitionTexture(tex, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, list);
    deferRelease(staging.p);
    staging.p = nullptr;
}

void RHI_D3D12::copyTexture(TextureHandle srcHandle, Uint32 srcMip, TextureHandle dstHandle, Uint32 dstMip) {
    TextureResource* src = resolveTexture(srcHandle.id);
    TextureResource* dst = resolveTexture(dstHandle.id);
    if (!src || !dst || src == dst) return;
    ID3D12GraphicsCommandList* list = activeList();

    transitionTexture(*src, D3D12_RESOURCE_STATE_COPY_SOURCE, list);
    transitionTexture(*dst, D3D12_RESOURCE_STATE_COPY_DEST, list);

    D3D12_TEXTURE_COPY_LOCATION s{};
    s.pResource = src->resource;
    s.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    s.SubresourceIndex = srcMip;
    D3D12_TEXTURE_COPY_LOCATION d{};
    d.pResource = dst->resource;
    d.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    d.SubresourceIndex = dstMip;
    list->CopyTextureRegion(&d, 0, 0, 0, &s, nullptr);

    // Contract: both textures shader-readable afterwards.
    const D3D12_RESOURCE_STATES readable = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                                           D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    transitionTexture(*src, readable, list);
    transitionTexture(*dst, readable, list);
}

void RHI_D3D12::flushUploads() {
    submitUploads(true);
}

void RHI_D3D12::unmapBuffer(BufferHandle handle) {
    // Persistent maps: nothing to do.
    (void)handle;
}

void* RHI_D3D12::mapBuffer(BufferHandle handle) {
    auto it = buffers.find(handle.id);
    if (it == buffers.end()) return nullptr;
    BufferResource& buf = it->second;
    if (!buf.mapped) return nullptr;
    // Swapchain readbacks land with 256-aligned rows; the renderer memcpy's
    // width*height*4, so compact once on first map.
    if (buf.readbackRowPitch && !buf.readbackCompacted) {
        const Uint32 tight = buf.readbackWidth * 4;
        if (buf.readbackRowPitch != tight) {
            Uint8* p = static_cast<Uint8*>(buf.mapped);
            for (Uint32 y = 1; y < buf.readbackHeight; y++) {
                std::memmove(p + size_t(y) * tight, p + size_t(y) * buf.readbackRowPitch, tight);
            }
        }
        buf.readbackCompacted = true;
    }
    return buf.mapped;
}

BufferHandle RHI_D3D12::copySwapchainToBuffer(Uint32& outWidth, Uint32& outHeight) {
    if (!insideFrame || !frameListOpen) return {};
    outWidth = swapchainWidth;
    outHeight = swapchainHeight;
    const Uint32 pitch = static_cast<Uint32>(alignUp(size_t(swapchainWidth) * 4,
                                                     D3D12_TEXTURE_DATA_PITCH_ALIGNMENT));

    BufferDesc bd;
    bd.size = size_t(pitch) * swapchainHeight;
    bd.usage = BufferUsage::TransferDst;
    bd.memoryUsage = MemoryUsage::GPUreadback;
    BufferHandle handle = createBuffer(bd);
    if (!handle.isValid()) return {};
    BufferResource& buf = buffers[handle.id];
    buf.readbackRowPitch = pitch;
    buf.readbackWidth = swapchainWidth;
    buf.readbackHeight = swapchainHeight;

    ID3D12Resource* backbuffer = backbuffers[backbufferIndex];
    D3D12_RESOURCE_BARRIER toCopy{};
    toCopy.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toCopy.Transition.pResource = backbuffer;
    toCopy.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    toCopy.Transition.StateBefore = backbufferState[backbufferIndex];
    toCopy.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    frameList->ResourceBarrier(1, &toCopy);

    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = backbuffer;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = buf.resource;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    dst.PlacedFootprint.Footprint.Width = swapchainWidth;
    dst.PlacedFootprint.Footprint.Height = swapchainHeight;
    dst.PlacedFootprint.Footprint.Depth = 1;
    dst.PlacedFootprint.Footprint.RowPitch = pitch;
    frameList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

    D3D12_RESOURCE_BARRIER back = toCopy;
    back.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    back.Transition.StateAfter = backbufferState[backbufferIndex];
    frameList->ResourceBarrier(1, &back);
    return handle;
}

// ============================================================================
// Mip generation: render-target downsample chain (matches vkCmdBlitImage's
// linear-filtered result). BGRA8/sRGB formats aren't UAV-writable, so a fullscreen
// draw per mip is the portable route.
// ============================================================================

static const char* kMipgenHlsl = R"(
Texture2DArray srcTex : register(t0);
SamplerState srcSampler : register(s0);
cbuffer MipParams : register(b0) { uint arraySlice; float2 invDstSize; }

void vs_main(uint vid : SV_VertexID, out float4 pos : SV_Position, out float2 uv : TEXCOORD0) {
    uv = float2((vid << 1) & 2, vid & 2);
    pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}

float4 ps_main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    return srcTex.SampleLevel(srcSampler, float3(uv, arraySlice), 0);
}
)";

void RHI_D3D12::createMipgenPipeline() {
    if (mipgenRootSig) return;

    D3D12_DESCRIPTOR_RANGE1 srvRange{};
    srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange.NumDescriptors = 1;
    srvRange.Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE;
    D3D12_ROOT_PARAMETER1 params[2] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[0].DescriptorTable.NumDescriptorRanges = 1;
    params[0].DescriptorTable.pDescriptorRanges = &srvRange;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[1].Constants.Num32BitValues = 3;

    D3D12_STATIC_SAMPLER_DESC samp{};
    samp.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    samp.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samp.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samp.MaxLOD = D3D12_FLOAT32_MAX;
    samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_VERSIONED_ROOT_SIGNATURE_DESC rs{};
    rs.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    rs.Desc_1_1.NumParameters = 2;
    rs.Desc_1_1.pParameters = params;
    rs.Desc_1_1.NumStaticSamplers = 1;
    rs.Desc_1_1.pStaticSamplers = &samp;
    DxPtr<ID3DBlob> blob, error;
    throwIfFailed(D3D12SerializeVersionedRootSignature(&rs, blob.put(), error.put()), "mipgen rootsig");
    throwIfFailed(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                              IID_PPV_ARGS(mipgenRootSig.put())),
                  "mipgen rootsig");

    auto compile = [&](const wchar_t* entry, const wchar_t* profile) {
        DxcBuffer src{};
        src.Ptr = kMipgenHlsl;
        src.Size = std::strlen(kMipgenHlsl);
        src.Encoding = DXC_CP_UTF8;
        LPCWSTR args[] = {L"-E", entry, L"-T", profile, L"-O3"};
        DxPtr<IDxcResult> result;
        throwIfFailed(dxcCompiler->Compile(&src, args, 5, nullptr, kIID_IDxcResult, result.putVoid()), "mipgen dxc");
        HRESULT status = S_OK;
        result->GetStatus(&status);
        throwIfFailed(status, "mipgen shader compile");
        DxPtr<IDxcBlob> object;
        result->GetOutput(DXC_OUT_OBJECT, kIID_IDxcBlob, object.putVoid(), nullptr);
        auto* p = static_cast<const Uint8*>(object->GetBufferPointer());
        return std::vector<Uint8>(p, p + object->GetBufferSize());
    };
    mipgenVsDxil = compile(L"vs_main", L"vs_6_0");
    mipgenPsDxil = compile(L"ps_main", L"ps_6_0");
}

ID3D12PipelineState* RHI_D3D12::getMipgenPso(DXGI_FORMAT rtvFormat) {
    auto it = mipgenPsoCache.find(rtvFormat);
    if (it != mipgenPsoCache.end()) return it->second;
    createMipgenPipeline();

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = mipgenRootSig;
    pd.VS = {mipgenVsDxil.data(), mipgenVsDxil.size()};
    pd.PS = {mipgenPsDxil.data(), mipgenPsDxil.size()};
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.SampleMask = UINT_MAX;
    pd.SampleDesc.Count = 1;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.NumRenderTargets = 1;
    pd.RTVFormats[0] = rtvFormat;
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    ID3D12PipelineState* pso = nullptr;
    if (FAILED(device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pso)))) {
        return nullptr;
    }
    // Cache owns the reference; released in shutdown via the cache sweep.
    mipgenPsoCache.emplace(rtvFormat, pso);
    return pso;
}

void RHI_D3D12::generateMipmaps(TextureHandle handle) {
    auto it = textures.find(handle.id);
    if (it == textures.end() || it->second.isView) return;
    TextureResource& tex = it->second;
    const Uint32 mips = std::max(1u, tex.desc.mipLevels);
    if (mips <= 1 || isDepthFormat(tex.desc.format) || tex.desc.sampleCount > 1) return;

    ID3D12PipelineState* pso = getMipgenPso(tex.resourceFormat);
    if (!pso) return;
    const Uint32 layers = std::max(1u, tex.desc.arrayLayers);

    auto record = [&](ID3D12GraphicsCommandList* list) {
        // Whole resource → PIXEL_SHADER_RESOURCE baseline, then per-mip flips.
        transitionTexture(tex, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, list);
        list->SetPipelineState(pso);
        list->SetGraphicsRootSignature(mipgenRootSig);
        // Mipgen samples through a static sampler and needs no sampler heap of
        // its own, but this lambda is also recorded onto the shared frameList
        // below. SetDescriptorHeaps replaces the entire set rather than adding
        // to it, so binding only the SRV heap here would unbind the sampler heap
        // beginFrame set, and every sampler root table for the rest of the frame
        // would fail with SET_DESCRIPTOR_TABLE_INVALID.
        ID3D12DescriptorHeap* heaps[] = {gpuSrvHeap.p, gpuSamplerHeap.p};
        list->SetDescriptorHeaps(2, heaps);
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        for (Uint32 layer = 0; layer < layers; layer++) {
            for (Uint32 mip = 1; mip < mips; mip++) {
                const Uint32 dstW = std::max(1u, tex.desc.width >> mip);
                const Uint32 dstH = std::max(1u, tex.desc.height >> mip);
                const UINT srcSub = (mip - 1) + layer * mips;
                const UINT dstSub = mip + layer * mips;

                D3D12_RESOURCE_BARRIER toRT{};
                toRT.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                toRT.Transition.pResource = tex.resource;
                toRT.Transition.Subresource = dstSub;
                toRT.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                                              D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
                toRT.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
                list->ResourceBarrier(1, &toRT);
                (void)srcSub;

                // Source mip via a transient shader-visible SRV
                Uint32 srvCpu = getOrCreateMipSrv(tex, layer, mip - 1);
                Uint32 ringSlot = allocRing(1);
                device->CopyDescriptorsSimple(1, gpuHeapCpuAt(ringSlot), cpuSrvHeap.cpuAt(srvCpu),
                                              D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
                list->SetGraphicsRootDescriptorTable(0, gpuHeapGpuAt(ringSlot));
                struct { Uint32 slice; float inv[2]; } consts{0, {1.0f / dstW, 1.0f / dstH}};
                list->SetGraphicsRoot32BitConstants(1, 3, &consts, 0);

                Uint32 rtv = getOrCreateRTV(tex, layer, mip);
                D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = cpuRtvHeap.cpuAt(rtv);
                list->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);
                D3D12_VIEWPORT vp{0, 0, float(dstW), float(dstH), 0, 1};
                D3D12_RECT sc{0, 0, LONG(dstW), LONG(dstH)};
                list->RSSetViewports(1, &vp);
                list->RSSetScissorRects(1, &sc);
                list->DrawInstanced(3, 1, 0, 0);

                D3D12_RESOURCE_BARRIER back = toRT;
                back.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
                back.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                                             D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
                list->ResourceBarrier(1, &back);
            }
        }
    };

    if (insideFrame && frameListOpen && !insideRenderPass && !insideComputePass) {
        record(frameList);
    } else if (!insideFrame) {
        // Loading path: uploads for mip 0 may still be queued — order them first.
        submitUploads(false);
        executeImmediate(record);
    } else {
        // Mid-pass call (renderer never does this today): defer to immediate.
        submitUploads(false);
        executeImmediate(record);
    }
}

// ============================================================================
// Attachment / mip views
// ============================================================================

Uint32 RHI_D3D12::getOrCreateRTV(TextureResource& tex, Uint32 layer, Uint32 mip) {
    const Uint64 key = (Uint64(layer) << 8) | mip;
    auto it = tex.rtvCache.find(key);
    if (it != tex.rtvCache.end()) return it->second;
    Uint32 index = cpuRtvHeap.alloc();
    D3D12_RENDER_TARGET_VIEW_DESC rtv{};
    rtv.Format = tex.resourceFormat;
    if (tex.desc.sampleCount > 1) {
        rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DMS;
    } else if (std::max(1u, tex.desc.arrayLayers) > 1) {
        rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
        rtv.Texture2DArray.MipSlice = mip;
        if (layer == 0xFFFFFFu) {
            rtv.Texture2DArray.FirstArraySlice = 0;
            rtv.Texture2DArray.ArraySize = tex.desc.arrayLayers;
        } else {
            rtv.Texture2DArray.FirstArraySlice = layer;
            rtv.Texture2DArray.ArraySize = 1;
        }
    } else {
        rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        rtv.Texture2D.MipSlice = mip;
    }
    device->CreateRenderTargetView(tex.resource, &rtv, cpuRtvHeap.cpuAt(index));
    tex.rtvCache.emplace(key, index);
    return index;
}

Uint32 RHI_D3D12::getOrCreateDSV(TextureResource& tex, Uint32 layer) {
    const Uint64 key = Uint64(layer) << 8;
    auto it = tex.dsvCache.find(key);
    if (it != tex.dsvCache.end()) return it->second;
    Uint32 index = cpuDsvHeap.alloc();
    D3D12_DEPTH_STENCIL_VIEW_DESC dsv{};
    dsv.Format = depthDsvFormat(tex.desc.format);
    if (tex.desc.sampleCount > 1) {
        dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DMS;
    } else if (std::max(1u, tex.desc.arrayLayers) > 1) {
        dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
        if (layer == 0xFFFFFFu) {
            dsv.Texture2DArray.FirstArraySlice = 0;
            dsv.Texture2DArray.ArraySize = tex.desc.arrayLayers;
        } else {
            dsv.Texture2DArray.FirstArraySlice = layer;
            dsv.Texture2DArray.ArraySize = 1;
        }
    } else {
        dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    }
    device->CreateDepthStencilView(tex.resource, &dsv, cpuDsvHeap.cpuAt(index));
    tex.dsvCache.emplace(key, index);
    return index;
}

Uint32 RHI_D3D12::getOrCreateMipSrv(TextureResource& tex, Uint32 layer, Uint32 mip) {
    const Uint64 key = (Uint64(layer) << 8) | mip;
    auto it = tex.mipSrvCache.find(key);
    if (it != tex.mipSrvCache.end()) return it->second;
    Uint32 index = cpuSrvHeap.alloc();
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = isDepthFormat(tex.desc.format) ? depthSrvFormat(tex.desc.format) : tex.resourceFormat;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    // Texture2DArray view even for plain 2D so the mipgen shader has one type.
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
    srv.Texture2DArray.MostDetailedMip = mip;
    srv.Texture2DArray.MipLevels = 1;
    srv.Texture2DArray.FirstArraySlice = layer == 0xFFFFFFu ? 0 : layer;
    srv.Texture2DArray.ArraySize = 1;
    device->CreateShaderResourceView(tex.resource, &srv, cpuSrvHeap.cpuAt(index));
    tex.mipSrvCache.emplace(key, index);
    return index;
}

// ============================================================================
// Frame operations
// ============================================================================

void RHI_D3D12::beginFrame() {
    ensureSwapchain();

    frameIndex = (frameIndex + 1) % MAX_FRAMES_IN_FLIGHT;
    waitForFenceValue(frameFenceValues[frameIndex]);
    retireZombies(fence->GetCompletedValue());

    // Queue any pending uploads ahead of this frame's commands.
    submitUploads(false);

    frameAllocators[frameIndex]->Reset();
    frameList->Reset(frameAllocators[frameIndex], nullptr);
    frameListOpen = true;
    insideFrame = true;

    backbufferIndex = swapchain->GetCurrentBackBufferIndex();
    ringCursor = 0;
    nextQuery = 0;
    framePasses[frameIndex].clear();

    ID3D12DescriptorHeap* heaps[] = {gpuSrvHeap.p, gpuSamplerHeap.p};
    frameList->SetDescriptorHeaps(2, heaps);

    graphicsDescriptorsDirty = true;
    graphicsPushDirty = true;
    computeDescriptorsDirty = true;
    computePushDirty = true;
    currentPipelineId = 0;
    currentComputePipelineId = 0;
}

void RHI_D3D12::endFrame() {
    if (!insideFrame) return;

    // Resolve this frame's timestamps into its readback slice.
    if (nextQuery > 0) {
        frameList->ResolveQueryData(timestampHeap, D3D12_QUERY_TYPE_TIMESTAMP,
                                    frameIndex * TIMESTAMP_QUERY_CAPACITY, nextQuery,
                                    timestampReadback,
                                    sizeof(Uint64) * frameIndex * TIMESTAMP_QUERY_CAPACITY);
    }

    // Present transition
    ID3D12Resource* backbuffer = backbuffers[backbufferIndex];
    if (backbufferState[backbufferIndex] != D3D12_RESOURCE_STATE_PRESENT) {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = backbuffer;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = backbufferState[backbufferIndex];
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        frameList->ResourceBarrier(1, &b);
        backbufferState[backbufferIndex] = D3D12_RESOURCE_STATE_PRESENT;
    }

    frameList->Close();
    frameListOpen = false;
    ID3D12CommandList* lists[] = {frameList.p};
    queue->ExecuteCommandLists(1, lists);

    swapchain->Present(1, 0);

    frameFenceValues[frameIndex] = nextFenceValue;
    // Zombies deferred while this frame was recording now belong to this
    // frame's signal (see deferRelease).
    for (Zombie& z : zombies) {
        if (z.fenceValue == kFencePendingFrame) z.fenceValue = nextFenceValue;
    }
    queue->Signal(fence, nextFenceValue++);
    insideFrame = false;

    resolveTimings();
}

void RHI_D3D12::beginRenderPass(const RenderPassDesc& desc) {
    if (!frameListOpen) return;
    insideRenderPass = true;
    beginPassTiming(desc.name ? desc.name : "RenderPass");

    std::array<D3D12_CPU_DESCRIPTOR_HANDLE, 8> rtvHandles{};
    Uint32 rtvCount = 0;
    passWidth = swapchainWidth;
    passHeight = swapchainHeight;

    for (size_t i = 0; i < desc.colorAttachments.size() && rtvCount < 8; i++) {
        const TextureHandle& att = desc.colorAttachments[i];
        D3D12_CPU_DESCRIPTOR_HANDLE handle{};
        if (att.id == 0) {
            // Swapchain drawable
            ID3D12Resource* backbuffer = backbuffers[backbufferIndex];
            if (backbufferState[backbufferIndex] != D3D12_RESOURCE_STATE_RENDER_TARGET) {
                D3D12_RESOURCE_BARRIER b{};
                b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                b.Transition.pResource = backbuffer;
                b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                b.Transition.StateBefore = backbufferState[backbufferIndex];
                b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
                frameList->ResourceBarrier(1, &b);
                backbufferState[backbufferIndex] = D3D12_RESOURCE_STATE_RENDER_TARGET;
            }
            handle = cpuRtvHeap.cpuAt(backbufferRtv[backbufferIndex]);
        } else {
            TextureResource* tex = resolveTexture(att.id);
            if (!tex) continue;
            transitionTexture(*tex, D3D12_RESOURCE_STATE_RENDER_TARGET, frameList);
            Uint32 layer = 0xFFFFFFu;
            Uint32 mip = 0;
            if (i == 0) {
                if (desc.colorArrayLayer != ~0u) layer = desc.colorArrayLayer;
                mip = desc.colorMipLevel;
            }
            handle = cpuRtvHeap.cpuAt(getOrCreateRTV(*tex, layer, mip));
            passWidth = std::max(1u, tex->desc.width >> mip);
            passHeight = std::max(1u, tex->desc.height >> mip);
        }
        rtvHandles[rtvCount++] = handle;

        const bool load = i < desc.loadColor.size() && desc.loadColor[i];
        if (!load) {
            glm::vec4 c = i < desc.clearColors.size() ? desc.clearColors[i] : glm::vec4(0);
            const float clearColor[4] = {c.r, c.g, c.b, c.a};
            frameList->ClearRenderTargetView(rtvHandles[rtvCount - 1], clearColor, 0, nullptr);
        }
    }

    D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle{};
    bool hasDepth = false;
    if (desc.depthAttachment.isValid()) {
        TextureResource* depthTex = resolveTexture(desc.depthAttachment.id);
        if (depthTex) {
            transitionTexture(*depthTex, D3D12_RESOURCE_STATE_DEPTH_WRITE, frameList);
            Uint32 layer = desc.depthArrayLayer != ~0u ? desc.depthArrayLayer : 0xFFFFFFu;
            dsvHandle = cpuDsvHeap.cpuAt(getOrCreateDSV(*depthTex, layer));
            hasDepth = true;
            if (desc.colorAttachments.empty()) {
                passWidth = depthTex->desc.width;
                passHeight = depthTex->desc.height;
            }
            if (!desc.loadDepth) {
                frameList->ClearDepthStencilView(dsvHandle, D3D12_CLEAR_FLAG_DEPTH,
                                                 desc.clearDepth,
                                                 static_cast<UINT8>(desc.clearStencil), 0, nullptr);
            }
        }
    }

    frameList->OMSetRenderTargets(rtvCount, rtvCount ? rtvHandles.data() : nullptr, FALSE,
                                  hasDepth ? &dsvHandle : nullptr);

    D3D12_VIEWPORT vp{0, 0, float(passWidth), float(passHeight), 0.0f, 1.0f};
    D3D12_RECT sc{0, 0, LONG(passWidth), LONG(passHeight)};
    frameList->RSSetViewports(1, &vp);
    frameList->RSSetScissorRects(1, &sc);

    // Stash for endRenderPass MSAA resolves
    currentPassDesc = desc;
    graphicsDescriptorsDirty = true;
    graphicsPushDirty = true;
    currentPipelineId = 0;
    vertexStreamDirty = true;
}

void RHI_D3D12::endRenderPass() {
    if (!insideRenderPass) return;

    // MSAA resolves
    for (size_t i = 0; i < currentPassDesc.resolveAttachments.size() &&
                       i < currentPassDesc.colorAttachments.size(); i++) {
        const TextureHandle& resolveTarget = currentPassDesc.resolveAttachments[i];
        const TextureHandle& msaaSource = currentPassDesc.colorAttachments[i];
        if (!resolveTarget.isValid() || msaaSource.id == 0) continue;
        TextureResource* src = resolveTexture(msaaSource.id);
        TextureResource* dst = resolveTexture(resolveTarget.id);
        if (!src || !dst) continue;
        transitionTexture(*src, D3D12_RESOURCE_STATE_RESOLVE_SOURCE, frameList);
        transitionTexture(*dst, D3D12_RESOURCE_STATE_RESOLVE_DEST, frameList);
        frameList->ResolveSubresource(dst->resource, 0, src->resource, 0, dst->resourceFormat);
        transitionTexture(*dst, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, frameList);
    }

    insideRenderPass = false;
    endPassTiming();
}

// ============================================================================
// Binding + descriptor flush
// ============================================================================

Uint32 RHI_D3D12::allocRing(Uint32 count) {
    if (ringCursor + count > RING_PER_FRAME) {
        // Wrapping mid-frame would overwrite live tables; that means the ring
        // is undersized for the scene — fail loudly.
        throw std::runtime_error("RHI_D3D12: per-frame descriptor ring exhausted");
    }
    Uint32 base = RING_BASE + frameIndex * RING_PER_FRAME + ringCursor;
    ringCursor += count;
    return base;
}

void RHI_D3D12::writeNullSrv(Uint32 dstIndex) {
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = DXGI_FORMAT_R32_TYPELESS;
    srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Buffer.NumElements = 1;
    srv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
    device->CreateShaderResourceView(nullptr, &srv, gpuHeapCpuAt(dstIndex));
}

void RHI_D3D12::writeNullUav(Uint32 dstIndex) {
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.Format = DXGI_FORMAT_R32_TYPELESS;
    uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    uav.Buffer.NumElements = 1;
    uav.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
    device->CreateUnorderedAccessView(nullptr, nullptr, &uav, gpuHeapCpuAt(dstIndex));
}

void RHI_D3D12::writeBufferSrv(Uint32 dstIndex, const BufferBinding& binding) {
    auto it = buffers.find(binding.id);
    if (it == buffers.end()) {
        writeNullSrv(dstIndex);
        return;
    }
    BufferResource& buf = it->second;
    if (binding.offset == 0) {
        device->CopyDescriptorsSimple(1, gpuHeapCpuAt(dstIndex), cpuSrvHeap.cpuAt(buf.srvIndex),
                                      D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        return;
    }
    // Offset binds (frame-slotted suballocations): raw view starting at offset.
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = DXGI_FORMAT_R32_TYPELESS;
    srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Buffer.FirstElement = binding.offset / 4;
    srv.Buffer.NumElements = static_cast<UINT>((buf.allocSize - binding.offset) / 4);
    srv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
    device->CreateShaderResourceView(buf.resource, &srv, gpuHeapCpuAt(dstIndex));
}

void RHI_D3D12::writeBufferUav(Uint32 dstIndex, const BufferBinding& binding) {
    auto it = buffers.find(binding.id);
    if (it == buffers.end() ||
        !(resourceDesc(it->second.resource).Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)) {
        writeNullUav(dstIndex);
        return;
    }
    BufferResource& buf = it->second;
    if (binding.offset == 0) {
        device->CopyDescriptorsSimple(1, gpuHeapCpuAt(dstIndex), cpuSrvHeap.cpuAt(buf.uavIndex),
                                      D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        return;
    }
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.Format = DXGI_FORMAT_R32_TYPELESS;
    uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    uav.Buffer.FirstElement = binding.offset / 4;
    uav.Buffer.NumElements = static_cast<UINT>((buf.allocSize - binding.offset) / 4);
    uav.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
    device->CreateUnorderedAccessView(buf.resource, nullptr, &uav, gpuHeapCpuAt(dstIndex));
}

Uint32 RHI_D3D12::getSamplerTable(const Uint32* samplerIds, Uint32 count) {
    Uint64 hash = fnv1a64(samplerIds, count * sizeof(Uint32));
    hash = fnv1a64(&count, sizeof(count), hash);
    auto it = samplerTableCache.find(hash);
    if (it != samplerTableCache.end()) return it->second;

    if (samplerHeapNext + count > SAMPLER_HEAP_SIZE) {
        throw std::runtime_error("RHI_D3D12: sampler heap exhausted (distinct sampler tables)");
    }
    Uint32 base = samplerHeapNext;
    samplerHeapNext += count;

    D3D12_SAMPLER_DESC fallback{};
    fallback.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    fallback.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    fallback.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    fallback.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    fallback.MaxLOD = D3D12_FLOAT32_MAX;
    for (Uint32 i = 0; i < count; i++) {
        auto sIt = samplers.find(samplerIds[i]);
        device->CreateSampler(sIt != samplers.end() ? &sIt->second.desc : &fallback,
                              samplerHeapCpuAt(base + i));
    }
    samplerTableCache.emplace(hash, base);
    return base;
}

void RHI_D3D12::flushGraphicsState() {
    auto pipeIt = pipelines.find(currentPipelineId);
    if (pipeIt == pipelines.end()) return;
    PipelineResource& pipe = pipeIt->second;

    if (graphicsDescriptorsDirty) {
        // set0/set1: [SRV x8][UAV x8] each
        Uint32 set0 = allocRing(BINDINGS_PER_SET * 2);
        Uint32 set1 = allocRing(BINDINGS_PER_SET * 2);
        for (Uint32 i = 0; i < BINDINGS_PER_SET; i++) {
            writeBufferSrv(set0 + i, boundVertexBuffers[i]);
            writeBufferUav(set0 + BINDINGS_PER_SET + i, boundVertexBuffers[i]);
            writeBufferSrv(set1 + i, boundFragmentBuffers[i]);
            writeBufferUav(set1 + BINDINGS_PER_SET + i, boundFragmentBuffers[i]);
        }
        // set2 textures + samplers
        Uint32 texBase = allocRing(TEXTURE_BINDINGS_PER_SET);
        Uint32 samplerIds[TEXTURE_BINDINGS_PER_SET];
        for (Uint32 i = 0; i < TEXTURE_BINDINGS_PER_SET; i++) {
            TextureResource* tex = boundTextures[i].texId ? resolveTexture(boundTextures[i].texId) : nullptr;
            auto texIt = textures.find(boundTextures[i].texId);
            if (tex && texIt != textures.end() && texIt->second.srvIndex != UINT32_MAX) {
                // Sampling inside a pass: resource must already be in a
                // shader-readable state (prepareTextureForSampling handles
                // compute-written ones; uploads left them readable).
                device->CopyDescriptorsSimple(1, gpuHeapCpuAt(texBase + i),
                                              cpuSrvHeap.cpuAt(texIt->second.srvIndex),
                                              D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            } else {
                writeNullSrv(texBase + i);
            }
            samplerIds[i] = boundTextures[i].samplerId;
        }
        Uint32 samplerBase = getSamplerTable(samplerIds, TEXTURE_BINDINGS_PER_SET);

        frameList->SetGraphicsRootDescriptorTable(kGfxRootSet0, gpuHeapGpuAt(set0));
        frameList->SetGraphicsRootDescriptorTable(kGfxRootSet1, gpuHeapGpuAt(set1));
        frameList->SetGraphicsRootDescriptorTable(kGfxRootTextures, gpuHeapGpuAt(texBase));
        frameList->SetGraphicsRootDescriptorTable(kGfxRootSamplers, samplerHeapGpuAt(samplerBase));
        if (boundBindlessTable) {
            auto tblIt = argumentTables.find(boundBindlessTable);
            if (tblIt != argumentTables.end()) {
                frameList->SetGraphicsRootDescriptorTable(kGfxRootBindless,
                                                          gpuHeapGpuAt(tblIt->second.baseSlot));
            }
        } else {
            // Root signature 1.1 volatile ranges tolerate an unset table only
            // if the shader never reads it; point it at heap start to stay
            // debug-layer clean.
            frameList->SetGraphicsRootDescriptorTable(kGfxRootBindless, gpuHeapGpuAt(0));
        }
        graphicsDescriptorsDirty = false;
    }

    if (graphicsPushDirty) {
        frameList->SetGraphicsRoot32BitConstants(kGfxRootConstants, 32, graphicsPushData, 0);
        graphicsPushDirty = false;
    }

    if (vertexStreamDirty) {
        for (Uint32 slot = 0; slot < 4; slot++) {
            if (!pendingVertexBuffers[slot].bufferId) continue;
            auto vbIt = buffers.find(pendingVertexBuffers[slot].bufferId);
            if (vbIt == buffers.end()) continue;
            D3D12_VERTEX_BUFFER_VIEW view{};
            view.BufferLocation = vbIt->second.va + pendingVertexBuffers[slot].offset;
            view.SizeInBytes = static_cast<UINT>(vbIt->second.allocSize - pendingVertexBuffers[slot].offset);
            view.StrideInBytes = pipe.vertexStride;
            frameList->IASetVertexBuffers(slot, 1, &view);
        }
        if (pendingIndexBuffer) {
            auto ibIt = buffers.find(pendingIndexBuffer);
            if (ibIt != buffers.end()) {
                D3D12_INDEX_BUFFER_VIEW view{};
                view.BufferLocation = ibIt->second.va + pendingIndexOffset;
                view.SizeInBytes = static_cast<UINT>(ibIt->second.allocSize - pendingIndexOffset);
                view.Format = DXGI_FORMAT_R32_UINT;
                frameList->IASetIndexBuffer(&view);
            }
        }
        vertexStreamDirty = false;
    }
}

void RHI_D3D12::bindPipeline(PipelineHandle pipeline) {
    auto it = pipelines.find(pipeline.id);
    if (it == pipelines.end() || !frameListOpen) return;
    if (currentPipelineId == pipeline.id) return;
    currentPipelineId = pipeline.id;
    frameList->SetPipelineState(it->second.pso);
    frameList->SetGraphicsRootSignature(graphicsRootSig);
    if (!it->second.isMesh) {
        frameList->IASetPrimitiveTopology(it->second.topology);
    }
    // New root signature binding invalidates tables + constants
    graphicsDescriptorsDirty = true;
    graphicsPushDirty = true;
    vertexStreamDirty = true;
}

void RHI_D3D12::bindVertexBuffer(BufferHandle buffer, Uint32 binding, size_t offset) {
    if (binding >= 4) return;
    pendingVertexBuffers[binding] = {buffer.id, offset};
    vertexStreamDirty = true;
}

void RHI_D3D12::bindIndexBuffer(BufferHandle buffer, size_t offset) {
    pendingIndexBuffer = buffer.id;
    pendingIndexOffset = offset;
    vertexStreamDirty = true;
}

void RHI_D3D12::setUniformBuffer(Uint32 set, Uint32 binding, BufferHandle buffer, size_t offset, size_t range) {
    (void)set;
    setVertexBuffer(binding, buffer, offset, range);
    setFragmentBuffer(binding, buffer, offset, range);
}

void RHI_D3D12::setStorageBuffer(Uint32 set, Uint32 binding, BufferHandle buffer, size_t offset, size_t range) {
    (void)set;
    setVertexBuffer(binding, buffer, offset, range);
    setFragmentBuffer(binding, buffer, offset, range);
}

void RHI_D3D12::setVertexBuffer(Uint32 binding, BufferHandle buffer, size_t offset, size_t range) {
    (void)range;
    if (binding >= BINDINGS_PER_SET) return;
    BufferBinding nb{buffer.id, offset};
    if (boundVertexBuffers[binding].id != nb.id || boundVertexBuffers[binding].offset != nb.offset) {
        boundVertexBuffers[binding] = nb;
        graphicsDescriptorsDirty = true;
    }
}

void RHI_D3D12::setFragmentBuffer(Uint32 binding, BufferHandle buffer, size_t offset, size_t range) {
    (void)range;
    if (binding >= BINDINGS_PER_SET) return;
    BufferBinding nb{buffer.id, offset};
    if (boundFragmentBuffers[binding].id != nb.id || boundFragmentBuffers[binding].offset != nb.offset) {
        boundFragmentBuffers[binding] = nb;
        graphicsDescriptorsDirty = true;
    }
}

void RHI_D3D12::setTexture(Uint32 set, Uint32 binding, TextureHandle texture, SamplerHandle sampler) {
    (void)set;
    if (binding >= TEXTURE_BINDINGS_PER_SET) return;
    TextureBinding nb{texture.id, sampler.id};
    if (boundTextures[binding].texId != nb.texId || boundTextures[binding].samplerId != nb.samplerId) {
        boundTextures[binding] = nb;
        graphicsDescriptorsDirty = true;
    }

    // Mirror setComputeSampledTexture: anything sampled by the pixel stage must
    // sit in a shader-resource state first. beginRenderPass leaves depth targets
    // in DEPTH_WRITE, and sampling a depth buffer in that state returns its
    // hardware-compressed representation rather than depth values — which is
    // what put the blocky black/white tiles over the scene.
    //
    // Attachments of the pass currently recording are excluded: they are
    // legitimately bound for write, and moving them to a read state here would
    // both break the pass and be an invalid barrier.
    TextureResource* tex = resolveTexture(texture.id);
    if (!tex || !frameListOpen) return;
    if (insideRenderPass) {
        if (currentPassDesc.depthAttachment.isValid() &&
            resolveTexture(currentPassDesc.depthAttachment.id) == tex) {
            return;
        }
        for (const TextureHandle& att : currentPassDesc.colorAttachments) {
            if (att.isValid() && resolveTexture(att.id) == tex) return;
        }
    }
    transitionTexture(*tex, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, frameList);
}

void RHI_D3D12::setVertexBytes(const void* data, size_t size, Uint32 binding) {
    // Same layout as rhi_vulkan.cpp: 16-byte slot at (binding%4)*16 in [0,64).
    if (!data || size == 0) return;
    Uint32 offset = (binding % 4) * 16;
    size_t clamped = std::min<size_t>(size, 64 - offset);
    std::memcpy(graphicsPushData + offset, data, clamped);
    graphicsPushDirty = true;
}

void RHI_D3D12::setFragmentBytes(const void* data, size_t size, Uint32 binding) {
    if (!data || size == 0) return;
    Uint32 offset = 64 + (binding % 4) * 16;
    size_t clamped = std::min<size_t>(size, 128 - offset);
    std::memcpy(graphicsPushData + offset, data, clamped);
    graphicsPushDirty = true;
}

// ============================================================================
// Draws
// ============================================================================

void RHI_D3D12::draw(Uint32 vertexCount, Uint32 instanceCount, Uint32 firstVertex, Uint32 firstInstance) {
    if (!insideRenderPass) return;
    flushGraphicsState();
    setBaseVertexConstants(static_cast<int32_t>(firstVertex), firstInstance);
    frameList->DrawInstanced(vertexCount, instanceCount, firstVertex, firstInstance);
}

void RHI_D3D12::drawIndexed(Uint32 indexCount, Uint32 instanceCount, Uint32 firstIndex,
                            int32_t vertexOffset, Uint32 firstInstance) {
    if (!insideRenderPass) return;
    flushGraphicsState();
    setBaseVertexConstants(vertexOffset, firstInstance);
    frameList->DrawIndexedInstanced(indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);
}

ID3D12CommandSignature* RHI_D3D12::getCommandSignature(Uint32 kind, Uint32 stride) {
    const Uint64 key = (Uint64(kind) << 32) | stride;
    auto it = commandSigCache.find(key);
    if (it != commandSigCache.end()) return it->second;

    D3D12_INDIRECT_ARGUMENT_DESC arg{};
    switch (kind) {
        case 0: arg.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED; break;
        case 1: arg.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW; break;
        case 2: arg.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH; break;
        default: arg.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_MESH; break;
    }
    D3D12_COMMAND_SIGNATURE_DESC sd{};
    sd.ByteStride = stride;
    sd.NumArgumentDescs = 1;
    sd.pArgumentDescs = &arg;
    ID3D12CommandSignature* sig = nullptr;
    // Draw-only signatures take no root signature.
    if (FAILED(device->CreateCommandSignature(&sd, nullptr, IID_PPV_ARGS(&sig)))) {
        return nullptr;
    }
    commandSigCache.emplace(key, sig);
    return sig;
}

void RHI_D3D12::drawIndexedIndirect(BufferHandle argsBuffer, size_t offset, Uint32 drawCount, Uint32 stride) {
    if (!insideRenderPass) return;
    auto it = buffers.find(argsBuffer.id);
    if (it == buffers.end()) return;
    // DrawCommand (graphics_gpu_structs.hpp) is layout-identical to
    // D3D12_DRAW_INDEXED_ARGUMENTS — the same 20-byte records Vulkan consumes.
    ID3D12CommandSignature* sig = getCommandSignature(0, stride);
    if (!sig) return;
    flushGraphicsState();
    frameList->ExecuteIndirect(sig, drawCount, it->second.resource, offset, nullptr, 0);
}

void RHI_D3D12::drawIndirect(BufferHandle argsBuffer, size_t offset, Uint32 drawCount, Uint32 stride) {
    if (!insideRenderPass) return;
    auto it = buffers.find(argsBuffer.id);
    if (it == buffers.end()) return;
    ID3D12CommandSignature* sig = getCommandSignature(1, stride);
    if (!sig) return;
    flushGraphicsState();
    frameList->ExecuteIndirect(sig, drawCount, it->second.resource, offset, nullptr, 0);
}

void RHI_D3D12::drawMeshTasks(Uint32 groupCountX, Uint32 groupCountY, Uint32 groupCountZ) {
    if (!insideRenderPass || !frameList6 || !capabilities.meshShaders) return;
    flushGraphicsState();
    frameList6->DispatchMesh(groupCountX, groupCountY, groupCountZ);
}

void RHI_D3D12::drawMeshTasksIndirect(BufferHandle argsBuffer, size_t offset) {
    if (!insideRenderPass || !frameList6 || !capabilities.meshShaders) return;
    auto it = buffers.find(argsBuffer.id);
    if (it == buffers.end()) return;
    ID3D12CommandSignature* sig = getCommandSignature(3, sizeof(Uint32) * 3);
    if (!sig) return;
    flushGraphicsState();
    frameList->ExecuteIndirect(sig, 1, it->second.resource, offset, nullptr, 0);
}

void RHI_D3D12::setScissor(int32_t x, int32_t y, Uint32 width, Uint32 height) {
    if (!insideRenderPass) return;
    D3D12_RECT sc{x, y, LONG(x + width), LONG(y + height)};
    frameList->RSSetScissorRects(1, &sc);
}

// ============================================================================
// Compute
// ============================================================================

void RHI_D3D12::beginComputePass(const char* name) {
    if (!frameListOpen) return;
    insideComputePass = true;
    beginPassTiming(name ? name : "Compute");
    frameList->SetComputeRootSignature(computeRootSig);
    computeDescriptorsDirty = true;
    computePushDirty = true;
    currentComputePipelineId = 0;
}

void RHI_D3D12::endComputePass() {
    if (!insideComputePass) return;
    insideComputePass = false;
    endPassTiming();
}

void RHI_D3D12::bindComputePipeline(ComputePipelineHandle pipeline) {
    auto it = computePipelines.find(pipeline.id);
    if (it == computePipelines.end() || !frameListOpen) return;
    if (currentComputePipelineId == pipeline.id) return;
    currentComputePipelineId = pipeline.id;
    frameList->SetPipelineState(it->second.pso);
}

void RHI_D3D12::setComputeBuffer(Uint32 binding, BufferHandle buffer, size_t offset, size_t range) {
    (void)range;
    if (binding >= BINDINGS_PER_SET) return;
    BufferBinding nb{buffer.id, offset};
    if (boundComputeBuffers[binding].id != nb.id || boundComputeBuffers[binding].offset != nb.offset) {
        boundComputeBuffers[binding] = nb;
        computeDescriptorsDirty = true;
    }
}

void RHI_D3D12::setComputeTexture(Uint32 binding, TextureHandle texture) {
    if (binding >= BINDINGS_PER_SET) return;
    TextureResource* tex = resolveTexture(texture.id);
    if (!tex) return;
    // Storage binds read/write in UAV state; legal here because compute passes
    // run outside render passes.
    transitionTexture(*tex, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, activeList());
    if (boundComputeImages[binding] != texture.id) {
        boundComputeImages[binding] = texture.id;
        computeDescriptorsDirty = true;
    }
}

void RHI_D3D12::setComputeSampledTexture(Uint32 binding, TextureHandle texture, SamplerHandle sampler) {
    if (binding >= BINDINGS_PER_SET) return;
    TextureBinding nb{texture.id, sampler.id};
    if (boundComputeSampled[binding].texId != nb.texId ||
        boundComputeSampled[binding].samplerId != nb.samplerId) {
        boundComputeSampled[binding] = nb;
        computeDescriptorsDirty = true;
    }
    TextureResource* tex = resolveTexture(texture.id);
    if (tex) {
        transitionTexture(*tex, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, activeList());
    }
}

void RHI_D3D12::setAccelerationStructure(Uint32 binding, AccelStructHandle accelStruct) {
    if (binding >= BINDINGS_PER_SET) return;
    if (boundComputeAccels[binding] != accelStruct.id) {
        boundComputeAccels[binding] = accelStruct.id;
        computeDescriptorsDirty = true;
    }
}

void RHI_D3D12::setComputeBytes(const void* data, size_t size, Uint32 binding) {
    // Same layout as rhi_vulkan.cpp: 16-byte slot at (binding%4)*16 in [0,64).
    if (!data || size == 0) return;
    Uint32 offset = (binding % 4) * 16;
    size_t clamped = std::min<size_t>(size, 64 - offset);
    std::memcpy(computePushData + offset, data, clamped);
    computePushDirty = true;
}

void RHI_D3D12::flushComputeState() {
    if (computeDescriptorsDirty) {
        Uint32 set0 = allocRing(BINDINGS_PER_SET * 2);
        for (Uint32 i = 0; i < BINDINGS_PER_SET; i++) {
            writeBufferSrv(set0 + i, boundComputeBuffers[i]);
            writeBufferUav(set0 + BINDINGS_PER_SET + i, boundComputeBuffers[i]);
        }
        Uint32 images = allocRing(BINDINGS_PER_SET);
        for (Uint32 i = 0; i < BINDINGS_PER_SET; i++) {
            auto texIt = textures.find(boundComputeImages[i]);
            if (texIt != textures.end() && texIt->second.uavIndex != UINT32_MAX) {
                device->CopyDescriptorsSimple(1, gpuHeapCpuAt(images + i),
                                              cpuSrvHeap.cpuAt(texIt->second.uavIndex),
                                              D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            } else {
                // Null TEXTURE UAV (not buffer): kernels declare RWTexture2D.
                D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
                uav.Format = DXGI_FORMAT_R32_FLOAT;
                uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
                device->CreateUnorderedAccessView(nullptr, nullptr, &uav, gpuHeapCpuAt(images + i));
            }
        }
        Uint32 sampled = allocRing(BINDINGS_PER_SET);
        Uint32 samplerIds[BINDINGS_PER_SET];
        for (Uint32 i = 0; i < BINDINGS_PER_SET; i++) {
            auto texIt = textures.find(boundComputeSampled[i].texId);
            if (texIt != textures.end() && texIt->second.srvIndex != UINT32_MAX) {
                device->CopyDescriptorsSimple(1, gpuHeapCpuAt(sampled + i),
                                              cpuSrvHeap.cpuAt(texIt->second.srvIndex),
                                              D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            } else {
                writeNullSrv(sampled + i);
            }
            samplerIds[i] = boundComputeSampled[i].samplerId;
        }
        Uint32 samplerBase = getSamplerTable(samplerIds, BINDINGS_PER_SET);

        Uint32 accels = allocRing(BINDINGS_PER_SET);
        for (Uint32 i = 0; i < BINDINGS_PER_SET; i++) {
            auto asIt = accelStructs.find(boundComputeAccels[i]);
            D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
            srv.ViewDimension = D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE;
            srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            if (asIt != accelStructs.end() && asIt->second.built) {
                srv.RaytracingAccelerationStructure.Location = asIt->second.currentVA();
                device->CreateShaderResourceView(nullptr, &srv, gpuHeapCpuAt(accels + i));
            } else {
                writeNullSrv(accels + i);
            }
        }

        frameList->SetComputeRootDescriptorTable(kCmpRootSet0, gpuHeapGpuAt(set0));
        frameList->SetComputeRootDescriptorTable(kCmpRootImages, gpuHeapGpuAt(images));
        frameList->SetComputeRootDescriptorTable(kCmpRootSampled, gpuHeapGpuAt(sampled));
        frameList->SetComputeRootDescriptorTable(kCmpRootSamplers, samplerHeapGpuAt(samplerBase));
        frameList->SetComputeRootDescriptorTable(kCmpRootAccel, gpuHeapGpuAt(accels));
        if (boundComputeBindlessTable) {
            auto tblIt = argumentTables.find(boundComputeBindlessTable);
            if (tblIt != argumentTables.end()) {
                frameList->SetComputeRootDescriptorTable(kCmpRootBindless,
                                                         gpuHeapGpuAt(tblIt->second.baseSlot));
            }
        } else {
            frameList->SetComputeRootDescriptorTable(kCmpRootBindless, gpuHeapGpuAt(0));
        }
        computeDescriptorsDirty = false;
    }
    if (computePushDirty) {
        frameList->SetComputeRoot32BitConstants(kCmpRootConstants, 16, computePushData, 0);
        computePushDirty = false;
    }
}

void RHI_D3D12::dispatch(Uint32 groupCountX, Uint32 groupCountY, Uint32 groupCountZ) {
    if (!insideComputePass) return;
    flushComputeState();
    frameList->Dispatch(groupCountX, groupCountY, groupCountZ);
}

void RHI_D3D12::dispatchIndirect(BufferHandle argsBuffer, size_t offset) {
    if (!insideComputePass) return;
    auto it = buffers.find(argsBuffer.id);
    if (it == buffers.end()) return;
    ID3D12CommandSignature* sig = getCommandSignature(2, sizeof(Uint32) * 3);
    if (!sig) return;
    flushComputeState();
    frameList->ExecuteIndirect(sig, 1, it->second.resource, offset, nullptr, 0);
}

void RHI_D3D12::computeBarrier() {
    // Compute write → any read. Buffers stay in implicit states, so a global
    // UAV barrier is the whole story.
    if (!frameListOpen) return;
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    frameList->ResourceBarrier(1, &b);
}

void RHI_D3D12::prepareTextureForSampling(TextureHandle texture) {
    TextureResource* tex = resolveTexture(texture.id);
    if (!tex || !frameListOpen) return;
    if (tex->state == D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
        D3D12_RESOURCE_BARRIER uav{};
        uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        uav.UAV.pResource = tex->resource;
        frameList->ResourceBarrier(1, &uav);
    }
    transitionTexture(*tex, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, frameList);
}

// ============================================================================
// Bindless texture tables
// ============================================================================

BufferHandle RHI_D3D12::createTextureArgumentTable(ShaderHandle fragmentShader, Uint32 bufferIndex,
                                                   Uint32 entryCount, Uint32 texturesPerEntry) {
    (void)fragmentShader;
    (void)bufferIndex;
    if (!capabilities.bindlessTextures) return {};
    const Uint32 capacity = entryCount * texturesPerEntry;
    if (bindlessNext + capacity > BINDLESS_REGION) {
        fmt::print(stderr, "RHI_D3D12: bindless region exhausted\n");
        return {};
    }
    ArgumentTable table;
    table.baseSlot = bindlessNext;
    table.capacity = capacity;
    table.texturesPerEntry = texturesPerEntry;
    bindlessNext += capacity;
    // Fill with nulls so unwritten slots are safely readable.
    for (Uint32 i = 0; i < capacity; i++) {
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MipLevels = 1;
        device->CreateShaderResourceView(nullptr, &srv, gpuHeapCpuAt(table.baseSlot + i));
    }
    Uint32 id = nextResourceId++;
    argumentTables.emplace(id, table);
    return BufferHandle{id};
}

void RHI_D3D12::writeTextureArgumentTable(BufferHandle table, Uint32 entry, Uint32 slot, TextureHandle texture) {
    auto tblIt = argumentTables.find(table.id);
    if (tblIt == argumentTables.end()) return;
    ArgumentTable& t = tblIt->second;
    const Uint32 index = entry * t.texturesPerEntry + slot;
    if (index >= t.capacity) return;
    auto texIt = textures.find(texture.id);
    TextureResource* tex = resolveTexture(texture.id);
    if (texIt == textures.end() || !tex || texIt->second.srvIndex == UINT32_MAX) return;
    device->CopyDescriptorsSimple(1, gpuHeapCpuAt(t.baseSlot + index),
                                  cpuSrvHeap.cpuAt(texIt->second.srvIndex),
                                  D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
}

void RHI_D3D12::bindTextureArgumentTable(BufferHandle table) {
    boundBindlessTable = table.id;
    graphicsDescriptorsDirty = true;
}

void RHI_D3D12::bindComputeTextureArgumentTable(BufferHandle table, Uint32 bufferIndex) {
    (void)bufferIndex;
    boundComputeBindlessTable = table.id;
    computeDescriptorsDirty = true;
}

// ============================================================================
// GPU timings
// ============================================================================

void RHI_D3D12::beginPassTiming(const char* name) {
    if (!gpuTimingEnabled || !frameListOpen) return;
    if (nextQuery + 2 > TIMESTAMP_QUERY_CAPACITY) return;
    TimedPass pass;
    pass.name = name;
    pass.beginQuery = nextQuery++;
    pass.endQuery = nextQuery++;
    openPassQuery = pass.endQuery;
    frameList->EndQuery(timestampHeap, D3D12_QUERY_TYPE_TIMESTAMP,
                        frameIndex * TIMESTAMP_QUERY_CAPACITY + pass.beginQuery);
    framePasses[frameIndex].push_back(std::move(pass));
}

void RHI_D3D12::endPassTiming() {
    if (!gpuTimingEnabled || !frameListOpen || openPassQuery == UINT32_MAX) return;
    frameList->EndQuery(timestampHeap, D3D12_QUERY_TYPE_TIMESTAMP,
                        frameIndex * TIMESTAMP_QUERY_CAPACITY + openPassQuery);
    openPassQuery = UINT32_MAX;
}

void RHI_D3D12::resolveTimings() {
    // Read the OLDEST frame slot — the one the fence just proved complete at
    // beginFrame. Its pass list is from MAX_FRAMES_IN_FLIGHT frames ago.
    const Uint32 oldest = (frameIndex + 1) % MAX_FRAMES_IN_FLIGHT;
    if (framePasses[oldest].empty() || !timestampFrequency) return;
    if (fence->GetCompletedValue() < frameFenceValues[oldest]) return;

    Uint8* mapped = nullptr;
    D3D12_RANGE range{sizeof(Uint64) * oldest * TIMESTAMP_QUERY_CAPACITY,
                      sizeof(Uint64) * (oldest + 1) * TIMESTAMP_QUERY_CAPACITY};
    if (FAILED(timestampReadback->Map(0, &range, reinterpret_cast<void**>(&mapped)))) return;
    const Uint64* stamps = reinterpret_cast<const Uint64*>(mapped) + size_t(oldest) * TIMESTAMP_QUERY_CAPACITY;

    resolvedTimings.clear();
    const double toMs = 1000.0 / double(timestampFrequency);
    Uint64 first = UINT64_MAX, last = 0;
    double busy = 0.0;
    Uint64 unionEnd = 0;
    for (const TimedPass& pass : framePasses[oldest]) {
        Uint64 b = stamps[pass.beginQuery];
        Uint64 e = stamps[pass.endQuery];
        if (e < b) continue;
        resolvedTimings.push_back({pass.name, double(e - b) * toMs});
        first = std::min(first, b);
        last = std::max(last, e);
        // Interval-union assuming in-order pass windows on one queue.
        Uint64 clampedBegin = std::max(b, unionEnd);
        if (e > clampedBegin) busy += double(e - clampedBegin) * toMs;
        unionEnd = std::max(unionEnd, e);
    }
    lastFrameSpanMs = (first != UINT64_MAX && last > first) ? double(last - first) * toMs : 0.0;
    lastFrameBusyMs = busy;
    D3D12_RANGE writtenNone{0, 0};
    timestampReadback->Unmap(0, &writtenNone);
    framePasses[oldest].clear();
}

std::vector<GpuPassTiming> RHI_D3D12::getGpuPassTimings() {
    return resolvedTimings;
}

// ============================================================================
// Backend queries + ImGui glue
// ============================================================================

void* RHI_D3D12::getBackendTexture(TextureHandle handle) const {
    auto it = textures.find(handle.id);
    if (it == textures.end()) return nullptr;
    if (it->second.isView) {
        auto src = textures.find(it->second.viewSourceId);
        return src != textures.end() ? src->second.resource.p : nullptr;
    }
    return it->second.resource.p;
}

bool RHI_D3D12::imguiInit(SDL_Window* sdlWindow) {
    if (!ImGui_ImplSDL3_InitForD3D(sdlWindow)) return false;
    ImGui_ImplDX12_InitInfo info;
    info.Device = device;
    info.CommandQueue = queue;
    info.NumFramesInFlight = MAX_FRAMES_IN_FLIGHT;
    info.RTVFormat = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    info.DSVFormat = DXGI_FORMAT_UNKNOWN;
    info.UserData = this;
    info.SrvDescriptorHeap = gpuSrvHeap;
    info.SrvDescriptorAllocFn = [](ImGui_ImplDX12_InitInfo* ii, D3D12_CPU_DESCRIPTOR_HANDLE* outCpu,
                                   D3D12_GPU_DESCRIPTOR_HANDLE* outGpu) {
        static_cast<RHI_D3D12*>(ii->UserData)->imguiAllocSrv(outCpu, outGpu);
    };
    info.SrvDescriptorFreeFn = [](ImGui_ImplDX12_InitInfo* ii, D3D12_CPU_DESCRIPTOR_HANDLE cpu,
                                  D3D12_GPU_DESCRIPTOR_HANDLE gpu) {
        static_cast<RHI_D3D12*>(ii->UserData)->imguiFreeSrv(cpu, gpu);
    };
    if (!ImGui_ImplDX12_Init(&info)) return false;
    imguiInitialized = true;
    return true;
}

void RHI_D3D12::imguiAllocSrv(D3D12_CPU_DESCRIPTOR_HANDLE* outCpu, D3D12_GPU_DESCRIPTOR_HANDLE* outGpu) {
    Uint32 slot;
    if (!imguiFree.empty()) {
        slot = imguiFree.back();
        imguiFree.pop_back();
    } else if (imguiNext < IMGUI_REGION) {
        slot = BINDLESS_REGION + imguiNext++;
    } else {
        slot = BINDLESS_REGION;  // out of slots: alias slot 0 rather than crash
    }
    *outCpu = gpuHeapCpuAt(slot);
    *outGpu = gpuHeapGpuAt(slot);
}

void RHI_D3D12::imguiFreeSrv(D3D12_CPU_DESCRIPTOR_HANDLE cpu, D3D12_GPU_DESCRIPTOR_HANDLE gpu) {
    (void)cpu;
    const Uint64 base = heapStartGpu(gpuSrvHeap.p).ptr;
    const Uint32 slot = static_cast<Uint32>((gpu.ptr - base) / srvStride);
    if (slot >= BINDLESS_REGION && slot < BINDLESS_REGION + IMGUI_REGION) {
        imguiFree.push_back(slot - BINDLESS_REGION);
    }
}

void RHI_D3D12::imguiRenderDrawData() {
    if (!imguiInitialized || !frameListOpen) return;
    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), frameList);
}

Uint64 RHI_D3D12::imguiTextureID(TextureHandle texture) {
    auto it = textures.find(texture.id);
    if (it == textures.end()) return 0;
    TextureResource& tex = it->second;
    if (tex.srvIndex == UINT32_MAX) return 0;
    if (tex.imguiSlot == UINT32_MAX) {
        D3D12_CPU_DESCRIPTOR_HANDLE cpu;
        D3D12_GPU_DESCRIPTOR_HANDLE gpu;
        imguiAllocSrv(&cpu, &gpu);
        const Uint64 base = heapStartGpu(gpuSrvHeap.p).ptr;
        tex.imguiSlot = static_cast<Uint32>((gpu.ptr - base) / srvStride) - BINDLESS_REGION;
    }
    const Uint32 slot = BINDLESS_REGION + tex.imguiSlot;
    device->CopyDescriptorsSimple(1, gpuHeapCpuAt(slot), cpuSrvHeap.cpuAt(tex.srvIndex),
                                  D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    return gpuHeapGpuAt(slot).ptr;
}

// Free-function glue (rhi_d3d12_imgui.hpp)
bool imguiD3D12Init(RHI* rhi, SDL_Window* window) {
    return static_cast<RHI_D3D12*>(rhi)->imguiInit(window);
}
void imguiD3D12Shutdown() {
    ImGui_ImplDX12_Shutdown();
    ImGui_ImplSDL3_Shutdown();
}
void imguiD3D12NewFrame() {
    ImGui_ImplDX12_NewFrame();
}
void imguiD3D12RenderDrawData(RHI* rhi) {
    static_cast<RHI_D3D12*>(rhi)->imguiRenderDrawData();
}
Uint64 imguiD3D12TextureID(RHI* rhi, TextureHandle texture) {
    return static_cast<RHI_D3D12*>(rhi)->imguiTextureID(texture);
}

// ============================================================================
// Factory
// ============================================================================

RHI* createRHID3D12() {
    return new RHI_D3D12();
}

} // namespace Vapor
