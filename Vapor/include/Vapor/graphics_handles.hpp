#pragma once
#include <SDL3/SDL_stdinc.h>
#include "rhi.hpp"

// ── GPU resource handles ──────────────────────────────────────────────────
// Standard handles (BufferHandle, TextureHandle, PipelineHandle, etc.) are
// defined in rhi.hpp. This file only defines additional application-layer handles.

// Template for application-layer handles
namespace Vapor {

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

// Application-layer handle (not an RHI handle)
struct AtlasHandleTag {};
using AtlasHandle = GPUHandle<AtlasHandleTag>;

} // namespace Vapor

// Transitional shim: these types lived at global scope before the namespace
// unification; unqualified call sites keep compiling while they migrate to
// Vapor:: qualification. Remove once call sites are migrated.
using namespace Vapor;
