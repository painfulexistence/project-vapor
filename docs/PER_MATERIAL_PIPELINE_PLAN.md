# Per-Material Pipeline Selection — Design Plan

## Why

project-vapor's Main pass binds a single `mainPipeline` (plus a bindless twin)
for every mesh, so every surface is shaded by the one PBR shader
(`RHIMain.frag` / `3d_pbr_normal_mapped.metal`). A faithful terrain (4-layer
splat + detail normals) and grass (instanced blades + wind) each need their
own surface shader, drawn with the Main pass's lighting/depth/shadow context —
not a bolt-on side pass that re-implements all of that.

Per-material pipeline selection — a material chooses which pipeline shades it,
draws grouped by pipeline — is a standard modern-renderer capability. This doc
plans it as the **foundation** the faithful TerrainStreaming port (and future
water, decals, etc.) builds on.

## Reference: how Atmospheric does it

Two-level dispatch (`Engine/src/renderer.cpp` `ForwardOpaquePass::Execute`):

- **Coarse — `MeshType` enum** (`mesh.hpp:16`, `PRIM/TERRAIN/SKY/VOXEL/GRASS/…`):
  a `switch (mesh->type)` binds a named shader program fetched once per pass
  (`terrain` / `grass` / `color`), from a string-keyed registry compiled at
  startup (`asset_manager.cpp:385` `LoadDefaultShaders`).
- **Fine — material subtype via `dynamic_cast`** (inside `PRIM`): `SkinnedMaterial`
  → `skinned`, `VATMaterial` → `vat`, else `color`. `TerrainMaterial`/
  `GrassMaterial` carry the type-specific inputs (splat map, `TerrainLayer[4]`,
  wind params) bound only in that branch.

Terrain/grass are submitted uniformly (a `MeshRendererComponent` like any other);
the type routing happens only in the pass switch. Program is re-bound per batch;
batches merge consecutive `(mesh, material)` (`BuildBatches`).

**Cleanliness cost to avoid:** Atmospheric duplicates the `switch`/`dynamic_cast`
in every pass (forward, deferred, shadow). We put the selector **on the material**
instead, so each pass consults one field. (Atmospheric's own WebGPU path already
trends this way: real pipeline objects + a `boundPipeline` de-dup guard.)

## project-vapor today (research findings)

`Vapor/src/renderer.cpp`:

- `mainRenderPass()` binds `mainPipeline` once (`:1668`), sets all common
  vertex/fragment/texture state (`:1670–1856`), then dispatches to one of four
  geometry paths: **BindlessMDI** (single submission, swaps to
  `mainPipelineBindless` `:1905`), **MDI** (one `drawIndexedIndirect` per material
  range in `m_materialRanges` `:1979`), **Indirect** and **CPU** (both group
  drawables into `materialBatches` by material `:1989`).
- **3 of 4 paths already sort/group by material and bind per-material textures at
  batch boundaries** (`bindMaterial()` `:6794`). Only the pipeline is never
  re-bound. Instance batching `stable_sort`s by `material` (`:1580`).
- A **variant pipeline is a 3-line copy-swap-create** of the bindless twin
  (`:5846`): copy `PipelineDesc`, swap `fragmentShader` (and/or vertex), create.
  All variants share vertex layout (`VertexData`), color formats
  (`RGBA16_FLOAT`), depth state, and the descriptor-set layout — so they are
  drop-in behind the common bindings.
- `RenderMaterial::pipeline` (`render_data.hpp:124`) already exists and is unused
  — the natural home for the selected pipeline handle.
- **Binding budget:** Vulkan `set2` fragment textures are at capacity (13/13,
  `RHIMain.frag`); Metal is far roomier. Terrain's detail textures need a
  strategy that doesn't need 8 more `set2` slots (see below).

## Design

### 1. Data model — a shader-variant selector on the material

- Add `enum class MaterialShader { Standard, Terrain, Grass, ... }` (default
  `Standard`) to `Material` (authoring) and mirror it on `RenderMaterial`.
- At material registration, map the variant → a `PipelineHandle` and store it in
  `RenderMaterial::pipeline` (or a `pipelineVariants[]` table indexed by the
  enum). `Standard` → `mainPipeline` as today.

### 2. Pipeline creation

- In `createRenderPipeline()`, after `mainPipeline`, build each variant with the
  copy-swap idiom (same `PipelineDesc`, swapped shaders). Terrain and grass share
  the vertex layout for now (grass instancing rides the existing instance buffer;
  a distinct grass vertex layout can come later if needed).

### 3. Draw integration (extend the batch key `material → (pipeline, material)`)

- **CPU + Indirect paths** (`:2004–2061`): `materialBatches` is already keyed by
  material; at the top of the per-material loop bind `pipelineFor(materialId)`
  before `bindMaterial`, tracking the last-bound pipeline to skip redundant binds.
  Re-binding mid-pass is safe (bindings are CPU-tracked and re-flushed; all
  variants share the descriptor layout).
- **MDI path** (`:1979`): same `bindPipeline` before `bindMaterial`; change the
  instance `stable_sort` (`:1580`) from `material` to `(pipeline, material)` so
  each pipeline group is a contiguous multi-draw span (make `m_materialRanges`
  pipeline-aware or add a parallel `m_pipelineRanges`).
- **BindlessMDI path** (`:1901`): a single submission that can't switch pipelines
  mid-call. **Phase 0: keep it single-pipeline (uber-shader) and route any
  non-`Standard` material through the MDI/Indirect/CPU path** (a material with a
  variant opts out of bindless). Splitting the single submission per pipeline is a
  later optimization, not a prerequisite.
- **Sort key** (`CalculateSortKey` `:361`): add a pipeline/variant field ranked
  above material so pipeline groups stay contiguous (keep depth ranking within a
  group for opaque front-to-back).

### 4. Terrain's detail textures (the binding-budget question)

The 4 detail layers (albedo+normal) are **shared globally across every terrain
tile** (world-space tiled), so they are not per-material — bind them **once when
the terrain pipeline is active**, not per draw. Options, in preference order:

- **(A) Two texture arrays**: pack the 4 albedo into one `2D array` and the 4
  normals into another → 2 bindings instead of 8. Splat params (weights computed
  in-shader) ride a small uniform. Fits even Vulkan's tight `set2`.
- **(B) A terrain-only descriptor set** bound only under the terrain pipeline.
- Splat weights are computed per-fragment from height/slope + world-space FBm
  (porting `defaultSplat` into the shader), so **no per-tile splat texture** is
  needed — this is what keeps terrain compatible with the fixed-slot streaming.

### 5. Migration (each step keeps the `Standard`/PBR path green)

1. Add `MaterialShader` enum + plumb to `RenderMaterial` (all `Standard`). No
   behavior change.
2. Build a second pipeline = exact copy of `mainPipeline`, select it for a test
   material, confirm output identical. Proves the seam.
3. Add `bindPipeline`-at-group-boundary to the CPU path, then Indirect, then MDI;
   exclude non-`Standard` from BindlessMDI. Regression: `Standard` scenes
   unchanged.
4. **Terrain variant** (Phase B rendering): terrain pipeline + `terrain.vert/.frag`
   (GLSL) + MSL twin + detail-texture-array binding.
5. **Grass variant** (Phase C rendering): grass pipeline + `grass.vert/.frag`,
   instanced.

## What Phase B / C can do NOW (independent of this foundation)

- **Phase A** — `TerrainTextureGen` (done, tested).
- **Terrain shaders authored** as files (`terrain.vert/.frag` GLSL + MSL twin):
  the splat-blend + in-shader-weight + normal-perturb algorithm is stable;
  only the final binding indices depend on step 4. Author now, finalize bindings
  when the foundation lands.
- **Data plumbing**: `StreamingTerrainComponent` carries the 4 `DetailLayer`s +
  splat params; `TerrainSystem` generates them via `TerrainTextureGen` and builds
  a `MaterialShader::Terrain` material. Rocks → spheres (add
  `MeshBuilder::buildSphere`). These compile and stage today; they simply shade
  as `Standard` until the terrain pipeline exists.
- **Grass placement** in `TerrainWorld` (CPU blade/cell generation + tests),
  fully independent of rendering.

## Scope

The per-material foundation (steps 1–3) is a self-contained, reviewable change
and a prerequisite for the terrain/grass **rendering**. Recommended order:
foundation → Phase B terrain rendering → Phase C grass. Tessellation stays out of
scope (skipped per request); the shader targets triangles directly.
