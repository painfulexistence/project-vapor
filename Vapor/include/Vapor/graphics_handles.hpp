#pragma once
#include <SDL3/SDL_stdinc.h>

// ── GPU resource handles ──────────────────────────────────────────────────
// Each handle is a distinct type wrapping a uint32 slot index.
// UINT32_MAX is the sentinel for "invalid / unallocated".
// The tag parameter makes handles incompatible at compile-time — passing a
// BufferHandle where a TextureHandle is expected is now a type error.

template<typename Tag> struct GPUHandle {
    Uint32 rid = UINT32_MAX;

    bool valid() const {
        return rid != UINT32_MAX;
    }

    bool operator==(const GPUHandle& other) const {
        return rid == other.rid;
    }
    bool operator!=(const GPUHandle& other) const {
        return rid != other.rid;
    }
};

struct PipelineHandleTag {};
struct BufferHandleTag {};
struct TextureHandleTag {};
struct RenderTargetHandleTag {};
struct RenderTextureHandleTag {};
struct AtlasHandleTag {};

using PipelineHandle = GPUHandle<PipelineHandleTag>;
using BufferHandle = GPUHandle<BufferHandleTag>;
using TextureHandle = GPUHandle<TextureHandleTag>;
using RenderTargetHandle = GPUHandle<RenderTargetHandleTag>;
using RenderTextureHandle = GPUHandle<RenderTextureHandleTag>;
using AtlasHandle = GPUHandle<AtlasHandleTag>;

// Render texture descriptor for creation
struct RenderTextureDesc {
    Uint32 width = 512;
    Uint32 height = 512;
    bool hasDepth = true;// Whether to include depth buffer
    bool hdr = false;// Use HDR format (RGBA16Float) vs LDR (RGBA8Unorm)
    Uint32 sampleCount = 1;// MSAA sample count (1 = no MSAA)
};
