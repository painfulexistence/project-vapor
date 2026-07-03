# Vision

Project Vapor is a current-gen 3D game engine I've been continuously building since 2024 Spring.
I have a clear vision for the engine and hope that this document can keep me anchored to that vision.

## The Three Pillars

### 1. ECS at scale, with breezy gameplay debugging

**The goal:** gameplay code that stays fast and stays debuggable as entity
counts grow — 100k+ live entities without the frame time or the developer's
mental model falling apart.

- Data-oriented entity storage (EnTT) as the single source of truth for world
  state. Systems are plain functions over component views; no God objects, no
  deep update hierarchies.
- Debugging is a first-class feature, not an afterthought: live entity
  inspection, component reflection, FSM state visualization, and scene
  serialization mean any weird behavior can be frozen, inspected, and replayed
  — not printf'd.
- The renderer consumes ECS data directly (transform/mesh/material components
  → GPU instance buffers) with no per-entity translation layer in between.


### 2. GPU-driven pipelines for rendering large scenes

**The goal:** rendering cost that scales with what's *visible*, not with what
*exists* — and a CPU that hands the GPU a scene, not a list of draw calls.

- Bindless resources: all textures and materials resident in argument-buffer
  heaps, indexed by ID from any shader. No per-draw texture binding.
- GPU culling: frustum and occlusion culling (depth pyramid, two-phase) run in
  compute; the CPU never iterates the scene for visibility.
- Indirect draws: culling results feed indirect command buffers; the entire
  opaque pass is a handful of encoder calls regardless of instance count.


### 3. Hybrid ray-traced rendering for high-fidelity visuals

**The goal:** rasterization for primary visibility, rays for what
rasterization fundamentally fakes — occlusion, shadows, reflections, and
eventually GI. Built for hardware RT (currently targeting Apple Silicon M3+).

- The G-buffer/prepass raster path stays; rays are dispatched from compute
  against a per-frame-refit TLAS.
- Every stochastic RT effect follows the same skeleton: few rays per pixel,
  blue-noise sampling, temporal accumulation, edge-aware spatial filtering.
- Ray shading uses the same bindless material heap as raster — pillar 2 is a
  prerequisite, not a sibling.
- RT AO, soft shadows, and reflections all should fit together in a ≤ 6 ms GPU budget at 1440p on M3, verified pass-by-pass in the GPU timing
panel.
