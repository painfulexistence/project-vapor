# Vision

Project Vapor is a current-gen 3D game engine built on three pillars. Every
feature on the roadmap should trace back to one of them; anything that doesn't
is a candidate for cutting.

## The three pillars

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

**Done looks like:** spawning 100k entities in the demo scene changes the
frame time by a predictable, linear amount, and any one of them can be
selected and fully inspected at runtime.

### 2. GPU-driven pipelines for rendering large scenes

**The goal:** rendering cost that scales with what's *visible*, not with what
*exists* — and a CPU that hands the GPU a scene, not a list of draw calls.

- Bindless resources: all textures and materials resident in argument-buffer
  heaps, indexed by ID from any shader. No per-draw texture binding.
- GPU culling: frustum and occlusion culling (depth pyramid, two-phase) run in
  compute; the CPU never iterates the scene for visibility.
- Indirect draws: culling results feed indirect command buffers; the entire
  opaque pass is a handful of encoder calls regardless of instance count.

**Done looks like:** a 10k+ instance scene renders through one indirect
opaque pass, and adding instances moves GPU compute time, not CPU encode time.

### 3. Hybrid ray-traced rendering for high-fidelity visuals

**The goal:** rasterization for primary visibility, rays for what
rasterization fundamentally fakes — occlusion, shadows, reflections, and
eventually GI. Built for Apple Silicon's hardware RT (M3+) with graceful
fallback where traversal is software (M1/M2).

- The G-buffer/prepass raster path stays; rays are dispatched from compute
  against a per-frame-refit TLAS.
- Every stochastic RT effect follows the same skeleton: few rays per pixel,
  blue-noise sampling, temporal accumulation, edge-aware spatial filtering
  (see ADR-008).
- Ray shading uses the same bindless material heap as raster — pillar 2 is a
  prerequisite, not a sibling.

**Done looks like:** RT AO, soft shadows, and reflections all fit together in
a ≤ 6 ms GPU budget at 1440p on M3, verified pass-by-pass in the GPU timing
panel.

## How the pillars reinforce each other

```
        ECS (world state)
         │  component views → instance/transform buffers, dirty tracking
         ▼
        GPU-driven pipeline (bindless heaps, GPU culling, indirect draws)
         │  bindless material access, stable instance IDs → TLAS refit
         ▼
        RT hybrid rendering (AO → shadows → reflections → GI)
```

ECS gives the renderer a flat, cache-friendly view of the world to extract
from. The GPU-driven layer turns that into resident GPU scene state — which is
exactly what ray tracing needs, because a ray can hit *anything*, so
*everything* must be shadeable without CPU intervention. The pillars are a
dependency chain, and the roadmap below is ordered by it.

## Non-goals

- **Not a general-purpose engine.** One renderer architecture, one target
  hardware family (Apple Silicon first; the Vulkan backend tracks the Metal
  feature set, it does not gate it).
- **Not photorealism at any cost.** Fidelity serves readability and mood;
  a stylized frame that holds 60 fps beats a cinematic one that doesn't.
- **Not editor-first.** Tooling grows out of debugging needs (inspection,
  timing, replay), not out of a scene-authoring GUI ambition.

## Rendering roadmap

Ordered by dependency, not by visual payoff. Each phase's output is the next
phase's input, and each item should land with a row in the GPU timing panel.

### Phase 0 — Pay down the tax (cleanups that unblock everything)
1. TLAS refit instead of per-frame rebuild; skip when nothing moved.
2. Move normal-matrix computation out of the vertex shader into InstanceData.
3. Replace the per-fragment MaterialData varyings with a material ID +
   fragment-side buffer fetch — this is the first step toward bindless
   materials, done inside the current binding model.
4. Motion vectors in the prepass — every temporal technique downstream
   (TAA, all RT denoisers) needs them; build once, reuse everywhere.

### Phase 1 — Bindless foundation
5. Global texture heap + material table in argument buffers
   (Metal 3 residency sets). All material textures resident; shaders index by
   material ID. Kills the per-draw `setFragmentTexture` loop.
6. TAA (consumes phase-0 motion vectors). Once stable, retire 4×MSAA — it
   returns bandwidth across the whole pipeline and removes the MS/resolve
   split that complicates every fullscreen pass.

### Phase 2 — GPU-driven geometry
7. ECS → GPU instance extraction: transform/mesh/material components written
   straight into a persistent instance buffer with dirty tracking.
8. GPU frustum culling in compute → indirect draw of the whole opaque pass
   (one `drawIndexedPrimitives` indirect per pipeline, no per-instance CPU
   loop, no `setVertexBytes`).
9. Depth-pyramid occlusion culling (two-phase: draw last frame's visible set,
   build pyramid, cull and draw the rest).

### Phase 3 — RT effects on the foundation
10. RT AO with the full denoise chain (raygen → temporal → à-trous →
    composite) per the ADR-008 plan — the cheapest RT effect, and it
    validates the shared denoiser infrastructure everything else reuses.
11. RT soft shadows: replace the 1-ray hard shadow with cone-sampled area
    light rays + the same denoiser; delete the shadow mip-gen blit.
12. RT reflections: needs bindless hit shading (phase 1) to evaluate
    materials at hit points. Half-res, roughness-cutoff, SSR fallback.
13. RT GI (probe-based first — DDGI-style irradiance volumes fed by the same
    TLAS; screen-space ReSTIR later if the budget allows).

### Phase 4 — Scale proof
14. A stress demo scene: 100k+ ECS entities, 10k+ visible instances, full RT
    effect stack, 60 fps on an M3 MacBook — the three pillars demonstrated in
    one frame, with the timing panel open.
