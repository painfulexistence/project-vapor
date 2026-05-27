# Project Vapor

[![CI](https://github.com/painfulexistence/project-vapor/actions/workflows/ci.yml/badge.svg)](https://github.com/painfulexistence/project-vapor/actions/workflows/ci.yml)
<br />

![C++](https://img.shields.io/badge/C%2B%2B-00599C?style=flat&logo=c%2B%2B&logoColor=white)
[![Follow on Twitter](https://img.shields.io/twitter/follow/DevLucidum.svg?style=social)](https://twitter.com/intent/follow?screen_name=DevLucidum)
[![Substack](https://img.shields.io/badge/Substack-%23006f5c.svg?style=flat&logo=substack&logoColor=ffffff&label=Arcane%20Realms)](https://painfulexistence.substack.com/)
<br />
<br />

A modern 3D game engine built from scratch in C++, targeting Metal and Vulkan. Solo project — GPU-driven rendering, ECS, physics, and a working demo game as the proving ground.

### Blog
[Arcane Realms](https://painfulexistence.substack.com/)

### On Agentic Coding

I don't like AI slop. Generated code that compiles but doesn't reflect intent, tests that pass without actually testing anything, architecture that looks right but collapses under real load — I've seen enough of it to be wary.

That said, I'm genuinely experimenting with agentic coding here, on my own terms.

The methodology has two pillars:

**Skill-based agents.** Rather than prompting freely, I've encoded disciplined workflows as reusable skills — composable scripts that agents execute consistently. `/deslop-cpp` audits the codebase for low-signal code: dead branches, defensive checks that can't trigger, comments that restate what the identifier already says, abstraction that serves no present requirement. `/safeguard` identifies behavioral gaps in the test suite and fills them with characterization tests that lock in real engine behavior. Agents follow these skills the same way every time, which makes their output predictable and auditable.

**Harness engineering.** Agents are most dangerous when they drift from your actual goals. I keep them aligned through two mechanisms. First, `docs/` is a living documentation site that always reflects the current state of the codebase — architecture, features, ADRs, tech stack decisions — updated in sync with code changes. Any agent working on this repo reads current docs, not stale ones, so its understanding of the engine matches mine. Second, `VISION.md` (internal) defines the engine's north star: what problems it's solving, what it is not, and what quality bar it needs to meet. Before any significant agent-driven work, the agent reads VISION.md to ensure the proposed changes are directionally correct.

The result is that I get the throughput benefits of agentic coding without ceding control over what the engine actually becomes.

### Main Features

#### Rendering
- Metal-cpp and Vulkan backends with feature parity tracking
- Physically-based rendering with Disney BRDF
- GLTF scene loading
- Tiled Forward rendering
- Raytraced hard shadow (Metal only)

#### Engine
- ECS architecture (EnTT) with a working game demo
- Jolt Physics integration
- RmlUi for in-game UI
- Async resource loading with cache deduplication
- ImGui scene inspector with per-component custom drawers
- MCP dev server for external tooling integration

### Getting Started

**Prerequisites**
- macOS with Xcode Command Line Tools
- CMake 3.24+
- Ninja (`brew install ninja`)
- ccache (`brew install ccache`)

**Configure**
```bash
cmake --preset dev
```

**Build & Run (Metal)**
```bash
cmake --build --preset dev --target main -j4
./build/Vaporware/Debug/main
```

**Build & Run (Vulkan)**
```bash
cmake --build --preset dev --target main -j4
./build/Vaporware/Debug/main --vulkan
```

### Testing

76 characterization tests across 5 binaries. These tests lock in the existing behavior of core engine systems — if a refactor silently breaks something, a test will fail.

```bash
cmake --build --preset dev --target test_action_system test_scene_transform test_camera test_physics test_resource_manager -j4
ctest --preset dev
```

Expected output: `100% tests passed, 0 tests failed out of 76`

| Binary | # | What it covers |
|--------|---|----------------|
| `test_action_system` | 37 | `ActionManager` start/stop/tag, `Timer` progress/reset, easing functions, `TimelineAction` sequencing, `ParallelAction`, instant-action chaining |
| `test_scene_transform` | 16 | Scene graph dirty propagation, world transform composition across 3-level hierarchies, `setLocalPosition/Scale`, `findNodeInHierarchy` |
| `test_camera` | 21 | Perspective/ortho projection matrices, `getViewMatrix` caching, frustum plane normals, `isVisible` for spheres and AABBs |
| `test_physics` | 1 | Jolt Physics body lifecycle: create → add → remove → destroy, no crash or assert |
| `test_resource_manager` | 2 | Async scene/image loading, cache deduplication (same path returns same handle) |

CI runs on every push via GitHub Actions (macOS 15, Ninja, ccache).

### Screenshots
#### Forward shading with tiled light culling
![tiled forward demo](.github/assets/tiled-forward-demo.png)
#### Raytraced hard shadow
![raytraced shadow demo](.github/assets/raytraced-shadow-demo.png)
