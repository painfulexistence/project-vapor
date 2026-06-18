# RHI (Render Hardware Interface) Documentation

## Table of Contents
1. [Architecture Overview](#architecture-overview)
2. [Design Decisions](#design-decisions)
3. [Implementation Guide](#implementation-guide)
4. [Migration Guide](#migration-guide)

---

## Architecture Overview

### Goal
Replace the old abstract Renderer base class with a concrete Renderer that uses an abstract RHI (Render Hardware Interface) layer underneath.

**Old Architecture:**
```
Application → Renderer (abstract) → Renderer_Metal / Renderer_Vulkan
```

**New Architecture:**
```
Application → Renderer (concrete) → RHI (abstract) → RHI_Metal / RHI_Vulkan
```

### Layer Responsibilities

#### Application Layer
- Scene management and ECS (EnTT)
- Game logic and entity systems
- High-level rendering requests

#### Renderer Layer (Concrete)
- Resource management (meshes, materials, textures)
- Frame orchestration (culling, sorting, batching)
- Multi-pass rendering logic (shadow, main, post-process)
- Lighting (clustered forward, deferred)

#### RHI Layer (Abstract)
- GPU API abstraction (Vulkan, Metal, D3D12)
- Low-level commands: draw, bind, barriers
- Resource creation: buffers, textures, pipelines
- Command buffer recording

---

## Design Decisions

### Why RHI Instead of Backend Inheritance?

**Advantages:**
1. **Single Implementation**: Only one Renderer class to maintain
2. **Separation of Concerns**: Rendering logic separate from GPU API
3. **Easier Testing**: Can mock RHI for unit tests
4. **Reduced Duplication**: Common code doesn't need to be in both backends

**Trade-offs:**
- Backend-specific optimizations require RHI extensions
- Some indirection overhead (negligible in practice)

### Branch Strategy
- **Main branch**: Old architecture (abstract Renderer)
- **RHI branch**: New architecture (concrete Renderer + abstract RHI)
- **Strategy**: Clean implementation on separate branch, then replace main

---

## Implementation Guide

### Core RHI Interface

```cpp
class RHI {
public:
    virtual ~RHI() = default;

    // Initialization
    virtual bool initialize(SDL_Window* window) = 0;
    virtual void shutdown() = 0;

    // Resource Creation
    virtual BufferHandle createBuffer(const BufferDesc& desc) = 0;
    virtual TextureHandle createTexture(const TextureDesc& desc) = 0;
    virtual PipelineHandle createPipeline(const PipelineDesc& desc) = 0;

    // Frame Operations
    virtual void beginFrame() = 0;
    virtual void endFrame() = 0;
    virtual void beginRenderPass(const RenderPassDesc& desc) = 0;
    virtual void endRenderPass() = 0;

    // Draw Commands
    virtual void bindPipeline(PipelineHandle pipeline) = 0;
    virtual void draw(uint32_t vertexCount, uint32_t instanceCount = 1) = 0;
    virtual void drawIndexed(uint32_t indexCount, uint32_t instanceCount = 1) = 0;

    // Screenshot Support
    virtual BufferHandle copySwapchainToBuffer(uint32_t& width, uint32_t& height) = 0;
    virtual void* mapBuffer(BufferHandle handle) = 0;
    virtual void unmapBuffer(BufferHandle handle) = 0;
};
```

### Renderer Implementation

```cpp
class Renderer {
public:
    void initialize(std::unique_ptr<RHI> rhiPtr, GraphicsBackend backend);
    void draw(std::shared_ptr<Scene> scene, Camera& camera);

private:
    std::unique_ptr<RHI> rhi;
    GraphicsBackend backend;

    // High-level resources
    std::vector<MeshResource> meshes;
    std::vector<MaterialResource> materials;
    std::vector<RenderTexture> textures;
};
```

### Creating a Renderer

```cpp
// Factory function
std::unique_ptr<Renderer> createRenderer(GraphicsBackend backend, SDL_Window* window) {
    std::unique_ptr<RHI> rhi;
    
    switch (backend) {
        case GraphicsBackend::Metal:
            rhi = std::make_unique<RHI_Metal>();
            break;
        case GraphicsBackend::Vulkan:
            rhi = std::make_unique<RHI_Vulkan>();
            break;
    }
    
    if (!rhi->initialize(window)) {
        return nullptr;
    }
    
    auto renderer = std::make_unique<Renderer>();
    renderer->initialize(std::move(rhi), backend);
    return renderer;
}
```

---

## Migration Guide

### For Application Code

**Before (Old API):**
```cpp
#include "Vapor/renderer_metal.hpp"
auto renderer = std::make_unique<Renderer_Metal>();
renderer->init(window);
renderer->draw(scene, camera);
renderer->deinit();
```

**After (New API):**
```cpp
#include "Vapor/renderer.hpp"
auto renderer = createRenderer(GraphicsBackend::Metal, window);
renderer->draw(scene, camera);
renderer->shutdown();
```

### Key Changes

1. **No more backend-specific includes** - Just include `renderer.hpp`
2. **Factory function** - Use `createRenderer()` instead of direct construction
3. **Initialization** - RHI initialized internally, no manual `init()` call
4. **Shutdown** - `deinit()` renamed to `shutdown()`

### Backend-Specific Features

If you need backend-specific functionality:

```cpp
#ifdef __APPLE__
if (backend == GraphicsBackend::Metal) {
    RHI_Metal* metalRHI = dynamic_cast<RHI_Metal*>(rhi.get());
    if (metalRHI) {
        // Access Metal-specific methods
        MTL::Device* device = metalRHI->getDeviceAs<MTL::Device>();
    }
}
#endif
```

### Resource Management

**Handles are now opaque:**
```cpp
// Old
VkBuffer buffer = renderer->getBuffer(...);

// New
BufferHandle buffer = renderer->createBuffer(...);
// Internal Vulkan buffer is hidden
```

---

## Implementation Status

### ✅ Completed
- [x] RHI interface design
- [x] RHI_Vulkan implementation
- [x] RHI_Metal implementation
- [x] Renderer refactoring to use RHI
- [x] Screenshot functionality via RHI
- [x] Batch rendering (2D/3D)
- [x] ImGui integration
- [x] Resource registration system
- [x] CI/CD pipeline updates

### 🚧 In Progress
- [ ] Compute shader support
- [ ] Ray tracing support
- [ ] Multi-threaded command recording

### 📋 Planned
- [ ] D3D12 backend
- [ ] WebGPU backend
- [ ] Render graph system
- [ ] GPU-driven rendering

---

## Performance Considerations

### RHI Overhead
The abstraction layer adds minimal overhead:
- Virtual function calls: ~1-2 CPU cycles per call
- Handle indirection: Negligible (simple ID lookup)
- Overall impact: <1% in typical rendering workloads

### Optimization Strategies
1. **Batch API calls** - Minimize RHI call count
2. **Resource caching** - Avoid redundant state changes
3. **Backend-specific paths** - Use `getBackendDevice()` for critical paths
4. **Command buffer reuse** - Record once, submit multiple times

---

## Troubleshooting

### Common Issues

**Q: Compilation fails with "pure virtual function called"**
A: Ensure all RHI methods are implemented in backend classes

**Q: Screenshots are black/corrupted**
A: Check that `copySwapchainToBuffer()` is called before `endFrame()`

**Q: ImGui not rendering**
A: Verify ImGui backend initialization order (NewFrame → Render → RenderDrawData)

**Q: Resource handles invalid**
A: Ensure resources aren't destroyed before use; check handle validity with `.isValid()`

### Debug Tips

```cpp
// Enable RHI validation layers
#define RHI_VALIDATION 1

// Log all RHI calls
#define RHI_TRACE_CALLS 1

// Check handle validity
if (!handle.isValid()) {
    fmt::print(stderr, "Invalid handle!\n");
}
```

---

## References

- Vulkan Spec: https://www.khronos.org/vulkan/
- Metal Best Practices: https://developer.apple.com/metal/
- EnTT Documentation: https://github.com/skypjack/entt
- RHI Design Patterns: Various game engine architectures (UE, Unity, etc.)

---

*Last updated: 2026-06-18*
