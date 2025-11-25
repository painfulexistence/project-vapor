# Renderer and RHI Layer Refactoring Plan

## Overview

This document describes the ongoing refactoring to separate the Renderer layer from the RHI (Render Hardware Interface) layer in Project Vapor.

## Goals

1. **Clear Separation of Concerns**
   - **RHI Layer**: Low-level GPU API wrapper (Vulkan, Metal)
   - **Renderer Layer**: High-level rendering logic (culling, sorting, draw calls)
   - **Application Layer**: Scene management and game logic

2. **Benefits**
   - Easier to maintain and test each layer independently
   - Simpler to add new rendering backends
   - GPU resource management centralized in RHI
   - Application layer doesn't know about GPU details

## Current Status

### ✅ Completed (Phase 1)

1. **Created RHI Interface** (`rhi.hpp`)
   - Defined abstract interface for GPU operations
   - Resource handle types (BufferHandle, TextureHandle, etc.)
   - Descriptor structures (BufferDesc, TextureDesc, etc.)
   - Enum types for resource properties

2. **Created Render Data Structures** (`render_data.hpp`)
   - Drawable structure for submitted rendering
   - RenderMesh, RenderMaterial, RenderTexture (renderer's internal representations)
   - Material flags and input structures
   - Camera and light data for rendering

3. **Created SceneRenderer Class** (`scene_renderer.hpp/cpp`)
   - High-level renderer that uses RHI
   - Resource registration (meshes, materials, textures)
   - Per-frame drawable submission
   - Culling and sorting logic
   - Separated from GPU API details

4. **Cleaned Up Application Layer**
   - **Scene**: Removed `vertices`, `indices`, `vertexBuffer`, `indexBuffer`
   - **Mesh**: Removed `vbos`, `ebo`, `instanceID`, `materialID`; added `rendererMeshId`
   - **Material**: Removed `pipeline`; added `rendererMaterialId`
   - **Image**: Removed `texture` GPU handle

### 🚧 In Progress (Phase 2)

**Refactor Vulkan Backend to RHI_Vulkan**

The current `Renderer_Vulkan` (2342 lines) mixes RHI-level code and high-level rendering logic. It needs to be split into:

- `RHI_Vulkan`: Implements the RHI interface
- Use `SceneRenderer` for high-level rendering

**Key Tasks:**

1. Create `rhi_vulkan.hpp` and `rhi_vulkan.cpp`
2. Move GPU resource management from `Renderer_Vulkan` to `RHI_Vulkan`
3. Implement all RHI interface methods:
   - `initialize()`, `shutdown()`
   - `createBuffer()`, `createTexture()`, `createShader()`, etc.
   - `beginFrame()`, `endFrame()`
   - `beginRenderPass()`, `endRenderPass()`
   - `bindPipeline()`, `bindVertexBuffer()`, `drawIndexed()`, etc.
4. Update command buffer recording to use RHI commands
5. Handle descriptor sets through RHI
6. Maintain synchronization (fences, semaphores)

**Challenges:**

- Descriptor set management needs careful abstraction
- Pipeline creation is complex (many states)
- Command buffer recording patterns
- Synchronization primitives
- ImGui integration (currently uses Vulkan directly)

### 📋 Planned (Phase 3)

1. **Refactor Metal Backend** (`renderer_metal.cpp` → `rhi_metal.cpp`)
   - Similar process as Vulkan
   - Implement RHI interface for Metal

2. **Update Application Layer** (`main.cpp`, asset loading)
   - Remove direct renderer usage
   - Use SceneRenderer for all rendering
   - Register scene resources with renderer
   - Submit drawables each frame

3. **Implement Material System**
   - Pipeline variant management
   - Shader permutations
   - Material property updates

4. **Advanced Features**
   - Batching and instancing
   - Indirect draw
   - GPU-driven culling
   - Multi-threading support

## Architecture Diagram

```
┌─────────────────────────────────────────────────┐
│              Application Layer                  │
│   - Game logic                                  │
│   - Scene management (CPU data only)            │
│   - Input handling                              │
└────────────────────┬────────────────────────────┘
                     │
┌────────────────────▼────────────────────────────┐
│           SceneRenderer (Renderer Layer)        │
│   - Resource registration                       │
│   - Drawable submission                         │
│   - Culling and sorting                         │
│   - Material binding                            │
│   - Uses RHI for all GPU operations             │
└────────────────────┬────────────────────────────┘
                     │
┌────────────────────▼────────────────────────────┐
│              RHI Layer                          │
│   - GPU resource creation/destruction           │
│   - Command buffer management                   │
│   - Descriptor set management                   │
│   - Synchronization                             │
└────────────────────┬────────────────────────────┘
                     │
              ┌──────┴──────┐
              │             │
       ┌──────▼─────┐ ┌─────▼──────┐
       │ RHI_Vulkan │ │ RHI_Metal  │
       │  Backend   │ │  Backend   │
       └────────────┘ └────────────┘
```

## Data Flow Example

### Resource Registration (Loading Time)

```
AssetManager::loadGLTF()
    ↓
Scene {
    nodes[], images[], materials[]  // CPU data only
}
    ↓
SceneRenderer::registerMesh(mesh.vertices, mesh.indices)
    ↓
RHI::createBuffer(vbDesc)
RHI::updateBuffer(vb, vertices)
    ↓
Returns MeshId → stored in mesh.rendererMeshId
```

### Frame Rendering (Runtime)

```
main loop:
    ↓
SceneRenderer::beginFrame(camera)
    ↓
For each visible node:
    SceneRenderer::submitDrawable({
        transform,
        mesh.rendererMeshId,
        material.rendererMaterialId
    })
    ↓
SceneRenderer::render()
    ├─ performCulling()
    ├─ sortDrawables()
    ├─ updateBuffers() → RHI::updateBuffer()
    └─ executeDrawCalls() → RHI::drawIndexed()
    ↓
SceneRenderer::endFrame()
```

## Migration Checklist

### For `renderer_vulkan.cpp` → `rhi_vulkan.cpp`

- [ ] Create RHI_Vulkan class implementing RHI interface
- [ ] Move Vulkan instance, device, surface creation
- [ ] Move swapchain management
- [ ] Move buffer creation/destruction
- [ ] Move texture creation/destruction
- [ ] Move shader module creation
- [ ] Move pipeline creation
- [ ] Move descriptor pool/set management
- [ ] Move command buffer recording
- [ ] Move synchronization (fences, semaphores)
- [ ] Handle frame-in-flight logic
- [ ] Update render pass management
- [ ] Integrate with SceneRenderer
- [ ] Update ImGui to use RHI (or keep as special case)
- [ ] Test basic rendering
- [ ] Test scene loading and rendering

### For Application Layer Updates

- [ ] Update main.cpp to use SceneRenderer
- [ ] Create helper functions to register scene with renderer
- [ ] Remove direct calls to Renderer::stage()
- [ ] Replace Renderer::draw() with SceneRenderer workflow
- [ ] Update asset_manager.cpp if needed
- [ ] Test full application flow

## Testing Strategy

1. **Unit Testing**
   - Test RHI buffer creation/update
   - Test resource handle management
   - Test descriptor set creation

2. **Integration Testing**
   - Test SceneRenderer with mock RHI
   - Test full scene registration
   - Test drawable submission and rendering

3. **System Testing**
   - Load and render Sponza scene
   - Verify lighting and materials
   - Check performance metrics
   - Test window resize
   - Test multiple frames

## Performance Considerations

- Buffer creation should be batched when possible
- Texture uploads can use staging buffers
- Descriptor sets should be cached
- Pipeline creation should be cached
- Command buffer recording can be parallelized (future)

## Notes

- Keep ImGui integration functional during refactor
- Maintain backward compatibility in intermediate steps
- Document any breaking changes
- Consider keeping old renderer temporarily for comparison

## References

- Original architecture analysis document
- Vulkan specification
- Metal API documentation

---

**Last Updated**: 2025-01-25
**Status**: Phase 1 Complete, Phase 2 In Progress
