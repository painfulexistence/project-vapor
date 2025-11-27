# Tracy Profiler Integration

This document describes the Tracy profiler integration in Project Vapor.

## What is Tracy?

Tracy is a real-time, nanosecond resolution, remote telemetry frame profiler for games and other applications. It provides detailed insights into CPU and GPU performance.

## Integration Points

Tracy profiling has been integrated into the following key areas:

### Engine Core (`engine_core.cpp`)
- `EngineCore::init()` - Engine initialization profiling
- `EngineCore::shutdown()` - Shutdown profiling
- `EngineCore::update()` - Per-frame update profiling

### Rendering (`renderer_vulkan.cpp`, `renderer_metal.cpp`)
- `Renderer::init()` - Renderer initialization
- `Renderer::stage()` - Scene staging
- `Renderer::draw()` - Main rendering loop (includes `FrameMark` for frame boundaries)

### Task Scheduler (`task_scheduler.cpp`)
- `TaskScheduler::init()` - Task system initialization
- `TaskScheduler::waitForAll()` - Task synchronization points

### Resource Manager (`resource_manager.cpp`)
- `loadImageInternal()` - Image loading (with file path names)
- `loadSceneInternal()` - Scene loading (with file path names)
- `loadMeshInternal()` - Mesh loading (with file path names)

## Usage

### Building with Tracy

Tracy is automatically linked when building the project. Make sure vcpkg has Tracy installed:

```bash
# Configure the project
cmake --preset dev

# Build the project
cmake --build build --config Debug
```

### Connecting Tracy Profiler

1. Download the Tracy profiler GUI from: https://github.com/wolfpld/tracy/releases
2. Run the Tracy profiler application
3. Launch your Vapor application
4. Connect to the application from the Tracy GUI

### Profiling Zones

The integration uses the following Tracy macros:
- `ZoneScoped` - Marks the entire function for profiling
- `ZoneName()` - Adds custom names to zones (e.g., file paths being loaded)
- `FrameMark` - Marks frame boundaries in the rendering loop

## Features

- **Real-time profiling**: See performance metrics as your application runs
- **Frame timing**: Track frame times and identify bottlenecks
- **Resource loading**: Monitor which resources are being loaded and how long they take
- **Thread visualization**: See how work is distributed across threads
- **Zone nesting**: Understand call hierarchies and where time is spent

## Further Reading

- Tracy Manual: https://github.com/wolfpld/tracy/releases/latest/download/tracy.pdf
- Tracy GitHub: https://github.com/wolfpld/tracy
