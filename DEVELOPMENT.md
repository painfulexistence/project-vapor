# Development Notes

## Project Status

**Current Milestone:** RHI Architecture Implementation  
**Active Branch:** `claude/split-renderer-rhi-layer-018QDXCch2WurFi2oiqLAz2T`  
**Last Updated:** 2026-06-18

---

## Recent Milestones

### 2026-06-18: RHI Implementation Complete ✅
- **Achievement**: Successfully implemented complete RHI layer with both Vulkan and Metal backends
- **Key Features**:
  - Screenshot functionality via `copySwapchainToBuffer()`
  - Buffer mapping for CPU readback
  - Scene models moved to application layer (Vaporware/)
  - Merged latest main branch changes
  
- **CI Status**: 
  - Fixed compilation errors (merge conflict markers)
  - Fixed Metal BufferResource initialization
  - All tests passing ✅

### 2026-05-28: RHI Phase Plan Complete
- Merged main branch successfully
- Resolved architecture conflicts
- Implemented shader path fixes
- Platform-specific compilation guards

### 2024-04-01: Project Inception 🎉
- Repository created
- Initial architecture planning

---

## Known Issues & Fixes

### Issue: CI Compilation Failures (2026-06-18)

**Symptoms:** All CI builds failing on both Ubuntu and macOS

**Root Causes:**
1. Unresolved merge conflict markers in `renderer.hpp`
   ```cpp
   <<<<<<< HEAD
   float time = 0.0f;
   =======
   bool isInitialized = false;
   >>>>>>> origin/main
   ```

2. Incomplete Metal BufferResource initialization
   ```cpp
   // Wrong: missing 4th field
   buffers[id] = {buffer, bufferSize, false};
   
   // Correct
   buffers[id] = {buffer, bufferSize, false, nullptr};
   ```

**Fix:** 
- Resolved conflict markers by merging both sets of members
- Added missing `nullptr` initializer for `mappedPointer`

---

## Development Workflow

### Branch Strategy
```
main (old architecture)
  └─> claude/split-renderer-rhi-layer-* (RHI refactor)
        └─> merge back to main when complete
```

### Commit Guidelines
- Use conventional commits: `feat:`, `fix:`, `refactor:`, `docs:`
- Include co-author tag: `Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>`
- Include session URL for tracking

### CI/CD
All PRs must pass:
- Build (Ubuntu: Debug, Release, MinSizeRel)
- Build (macOS: Debug, Release, MinSizeRel)
- Unit Tests (macOS / Clang)
- Screenshot Test (Renderer - Screenshot Capture)

---

## Testing Strategy

### Unit Tests
```bash
# Build and run tests
cmake --build build --target backend_screenshot_test
./build/tests/backend_screenshot_test
```

### Screenshot Test
Validates RHI screenshot functionality:
1. Creates renderer with backend (Metal/Vulkan)
2. Calls `readPixelsAsync()` with callback
3. Renders dummy frame
4. Waits for callback with image data
5. Saves `test_baseline.png`

**Expected behavior:** Test passes, PNG saved successfully

### Manual Testing
```bash
# Run main application
./build/Vaporware/Vaporware

# Check ImGui panels work
# Verify rendering on both backends
# Test model loading from Vaporware/assets/
```

---

## Architecture Evolution

### Phase 1: Analysis (Complete)
- Analyzed main branch renderer architecture
- Identified abstraction layer issues
- Decided on RHI approach over backend inheritance

### Phase 2: Design (Complete)
- Designed RHI interface
- Planned backend implementations
- Created migration guide

### Phase 3: Implementation (Complete)
- Implemented RHI_Vulkan
- Implemented RHI_Metal
- Refactored Renderer to use RHI
- Added screenshot support
- Integrated with EnTT ECS

### Phase 4: Testing & Refinement (In Progress)
- CI pipeline passing ✅
- Screenshot tests working ✅
- Performance validation
- Code review

### Phase 5: Merge to Main (Next)
- Final testing
- Update main branch README
- Archive old implementation

---

## Code Organization

### Engine Layer (Vapor/)
```
Vapor/
├── include/Vapor/
│   ├── rhi.hpp                 # RHI interface
│   ├── rhi_vulkan.hpp          # Vulkan backend
│   ├── rhi_metal.hpp           # Metal backend
│   ├── renderer.hpp            # Concrete renderer
│   ├── graphics.hpp            # Core graphics types
│   └── components.hpp          # ECS components
└── src/
    ├── rhi_vulkan.cpp
    ├── rhi_metal.cpp
    └── renderer.cpp
```

### Application Layer (Vaporware/)
```
Vaporware/
├── src/
│   └── main.cpp               # Application entry
└── assets/
    ├── models/                # Scene models (Sponza, etc.)
    ├── shaders/               # Application shaders
    └── textures/              # Application textures
```

---

## Performance Notes

### Batch Rendering
- 2D batching: Up to 10K quads per batch
- 3D batching: Instanced rendering for similar objects
- Auto-flush when batch full or material change

### Memory Usage
- GPU memory pooling via RHI
- Staging buffer reuse for uploads
- Texture atlas for UI (planned)

### Frame Timing
- Target: 60 FPS (16.67ms)
- CPU: ~5-8ms (culling, sorting, batching)
- GPU: ~8-10ms (rendering)
- Vsync: remainder

---

## Dependencies

### Core
- C++20
- CMake 3.20+
- SDL3
- EnTT (ECS)
- GLM (math)
- fmt (formatting)

### Graphics
- Vulkan SDK 1.3+ (Linux/Windows)
- Metal (macOS)
- metal-cpp (Metal C++ bindings)

### Third-party
- stb_image (image loading)
- tinygltf (GLTF loading)
- imgui (UI)
- FFmpeg (video encoding)

---

## Future Work

### Short Term
- [ ] Optimize culling (spatial partitioning)
- [ ] Add GPU profiling markers
- [ ] Implement render graph

### Medium Term
- [ ] Multi-threaded command recording
- [ ] Async resource loading
- [ ] Texture streaming

### Long Term
- [ ] D3D12 backend
- [ ] Ray tracing integration
- [ ] GPU-driven rendering
- [ ] Nanite-style virtual geometry

---

## Debugging Tips

### Enable Validation Layers
```cpp
// Vulkan
export VK_LAYER_PATH=/path/to/vulkan/layers
export VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation

// Metal
export METAL_DEVICE_WRAPPER_TYPE=1
```

### Common Pitfalls

**1. Shader Path Issues**
```cpp
// Wrong: includes 'assets/' prefix
readFile("assets/shaders/pbr.vert.spv")

// Correct: FileSystem searches Res/ automatically
readFile("shaders/pbr.vert.spv")
```

**2. Resource Lifetime**
```cpp
// Wrong: handle destroyed before use
{
    auto buffer = rhi->createBuffer(...);
}
rhi->bindBuffer(buffer); // Invalid!

// Correct: keep handle alive
auto buffer = rhi->createBuffer(...);
rhi->bindBuffer(buffer);
// ... later
rhi->destroyBuffer(buffer);
```

**3. Platform Guards**
```cpp
// Always guard Metal code
#ifdef __APPLE__
    #include "rhi_metal.hpp"
    // Metal-specific code
#endif
```

---

## Resources

### Documentation
- [RHI_DOCUMENTATION.md](RHI_DOCUMENTATION.md) - Complete RHI guide
- [README.md](README.md) - Project overview
- [TECHSTACK.md](TECHSTACK.md) - Technology choices
- [ROADMAP.md](ROADMAP.md) - Future plans

### External Links
- [EnTT Wiki](https://github.com/skypjack/entt/wiki)
- [Vulkan Tutorial](https://vulkan-tutorial.com/)
- [Metal Programming Guide](https://developer.apple.com/metal/)
- [Learn OpenGL](https://learnopengl.com/) (concepts apply to all APIs)

---

## Contact & Contribution

This is a personal learning project. For questions or suggestions:
- Open an issue on GitHub
- Check existing documentation
- Review recent commits for context

---

*Maintained by: Project Vapor Development Team*  
*Last Updated: 2026-06-18*
