# MicroVoxel Port Plan — Atmospheric → Project Vapor

Status: **proposal** (no implementation yet)
Target branch: `claude/microvoxel-project-vapor-port-ng9trx`
Source of truth studied: `Atmospheric/Examples/MicroVoxel` + `Atmospheric/Engine`
(`micro_voxel_pass.cpp`, `voxel_volume_component.cpp`, `microvoxel*.frag/vert`)
and the current Vapor RHI renderer.

> Fun historical note: `Atmospheric/Engine/src/voxel_volume_component.cpp:14-16`
> says its DDA/generator were originally ported *from* project-vapor's Metal
> renderer. That code no longer exists in this tree — this port brings the
> feature home, rebuilt on the RHI renderer.

---

## 1. What MicroVoxel is (as shipped in Atmospheric)

A raymarched micro-voxel diorama renderer — **no triangles**:

- `VoxelVolumeComponent`: dense 256³ grid of 1-byte palette indices
  (5 cm voxels → 12.8 m cube, 16 MiB), byte-per-8³-brick occupancy grid
  (32³ = 32 KiB), 256×2 RGBA8 palette (row 0 albedo+emission, row 1
  reflectivity/roughness). CPU procedural gen (gradient-fBm terrain, fBm
  caves, ore, floating crystals, emissive glowstone), `CarveSphere` editing,
  `RaycastVoxel` aiming. Whole-volume GPU re-upload on any edit.
- `MicroVoxelPass` (OpenGL 4.1): per-volume AABB box raster → fragment
  two-level DDA (brick skip + voxel walk, Amanatides–Woo), writes
  `gl_FragDepth` so voxels depth-composite with the raster scene. Shading:
  face normals, Minecraft-style corner AO (8 taps), sun + shadow ray, ≤4
  point lights (optional shadow rays), palette emission, Fresnel-blended
  mirror reflections.
- GI: half-res fullscreen pass re-traces the primary ray, adds 1
  cosine-weighted bounce (+1 sun-shadow ray at the bounce hit), temporal
  accumulation via world-space reprojection with distance-based history
  validation (blend 0.93), then N=3 à-trous (SVGF-lite) iterations. GI
  stores incident radiance; composite multiplies by albedo.
- Demo app: 3 independent volumes side by side, warm sun, warm point
  light, bloom, dig-with-E, debug views 0–6, toggles G/O/H/P/X/N/V/B.

Perf-relevant limits of the original: fragment-only (GL 4.1 ceiling on
macOS), integer 3D textures, dense storage (world size hard-capped by
memory), GI re-traces primary visibility at half res, whole-volume
re-upload + full GI history reset on every carve, ≤4 volumes in GI,
4 hard-coded point lights.

## 2. Goals for the port

1. **Beat the OpenGL version's performance** on the same content.
2. **Finer detail** — support 2.5 cm voxels in the hero demo, richer
   materials (engine IBL sky, clustered lights, HDR pipeline).
3. **Much larger world** — streaming, sparse storage, LOD; hundreds of
   meters instead of a 12.8 m diorama.
4. Follow Vapor's three pillars (VISION.md): ECS-driven, GPU-driven,
   and the stochastic-effect skeleton (few rays/pixel, temporal
   accumulation, edge-aware spatial filtering) that the GI already matches.

## 3. Where it lands in Vapor

| Piece | Location | Pattern followed |
|---|---|---|
| `VoxelVolumeComponent` | `Vapor/include/Vapor/components.hpp` | plain EnTT struct, `Hidden<>` for non-inspector state |
| `VoxelVolumeSystem` | `Vapor/include/Vapor/systems.hpp` | `SkySystem`/`WindSystem`: resolve component → push to renderer via `IRenderer` virtuals |
| Voxel world data (bricks, page table, palette, streaming, edits) | new `Vapor/src/voxel_world.cpp` + `include/Vapor/voxel_world.hpp` | CPU source of truth; enkiTS jobs via `EngineCore::getTaskScheduler()` |
| Render passes | `Renderer` (RHI path only) in `Vapor/src/renderer.cpp`, added in `setupDefaultRenderGraph` | `render_graph.hpp` `addPass(...)`; native `Renderer_Metal` intentionally skips the feature |
| Shaders | `Vapor/assets/shaders/MicroVoxel*.{vert,frag,comp}` + `3d_microvoxel*.metal` | GLSL/MSL twins with mirrored binding contracts (repo convention) |
| Demo | new top-level `Examples/MicroVoxel/` (guarded by `PROJECT_IS_TOP_LEVEL`) | mirrors Atmospheric's `Examples/MicroVoxel` structure; a lean `main.cpp` (no UI pages/physics/GLTF) |
| Tests | `tests/test_voxel_world.cpp` | Catch2, pure-logic (no GPU) |

`IRenderer` gains no-op-defaulted virtuals (`setVoxelWorld`,
`updateVoxelSettings`) so `Renderer_Metal` and the particle-test mock
renderer stay source-compatible.

## 4. Modernized data structure (the heart of "larger world")

Replace *dense 3D texture per volume* with a **sparse chunked brick pool**,
all in storage buffers (the RHI has no integer texture formats, and buffers
enable per-brick updates + bitmask DDA):

```
World (one per VoxelVolumeComponent)
 └─ Chunk grid: 128³-voxel chunks (6.4 m at 5 cm), streamed around camera
     └─ Page table: per chunk, 16³ brick entries → u32 brick slot
        (sentinels: EMPTY, UNIFORM(material))
         └─ Brick pool (one global StorageBuffer):
             • occupancy: 8×u64 bitmask per 8³ brick   (64 B)
             • materials: 512 × u8 palette indices     (512 B)
```

- **3-level DDA**: chunk step (page-table presence) → brick step (slot
  lookup) → voxel walk. The voxel walk tests the **bitmask first** (1–2
  u64 loads per 4×4×4 region) and fetches the material byte only on hit —
  strictly fewer memory transactions per step than the original's
  `texelFetch` per voxel, on top of skipping empty chunks entirely.
- **Sparsity math**: terrain-like content occupies ~10–25 % of bricks. A
  **512 MiB pool budget** ≈ 930 K resident bricks ≈ 476 M solid-capable
  voxels — a ~300×64×300 m sparse world at 5 cm, vs. 12.8 m dense. Pool
  size is a component knob; slots are free-listed and recycled on evict.
- **LOD bricks**: each chunk keeps a ¼-res mip brick set; rays switch to
  coarse voxels past a distance threshold (voxel size doubles), bounding
  per-pixel step counts for long views. (Phase 3.)
- **Per-brick dirty upload**: `CarveSphere` touches only overlapped bricks;
  uploads go through `rhi->updateBuffer` on those slots — no whole-volume
  re-upload, no GI history nuke (temporal distance validation already
  rejects stale samples at edited surfaces).
- **CPU stays source of truth** (needed by `RaycastVoxel`, carving, future
  physics). Generation is the Atmospheric noise stack (hash / value /
  gradient fBm — port verbatim for determinism), parallelized per chunk
  column via enkiTS `ITaskSet`, streamed in asynchronously; uploads ride
  the RHI's batched upload stream. GPU compute generation is a Phase-4
  option behind the same page-table contract.

## 5. Modernized render pipeline

Passes inserted into `setupDefaultRenderGraph` **between `Main` and
`SkyAtmosphere`** (so sky, fog, clouds, god-rays automatically treat
voxels as scene geometry), except GI trace/denoise which run before the
shade pass at half res:

```
MicroVoxelPrimary   box-raster per volume AABB (cull off), 3-level DDA
                    → writes voxel G-buffer: hitT (R32F), normal+material
                      (RGBA8), and DEPTH into depthStencilRT
                      (depthWrite on, CompareOp::Less; discard on miss)
MicroVoxelGI        [RequiresCompute] half-res: reads voxel G-buffer
                    (no primary re-trace — the big win over GL), 1
                    cosine-weighted bounce + sun-shadow ray at bounce hit,
                    temporal accumulation (world-space reprojection +
                    distance validation, blend 0.93)
MicroVoxelAtrous ×N [RequiresCompute] 5×5 à-trous, edge-stopped by
                    normal/depth/luma (ping-pong)
MicroVoxelShade     fullscreen frag on voxel-covered pixels (stencil or
                    material>0 test): corner AO (8 taps), sun via
                    atmosphereData + DDA shadow ray, clustered point/spot
                    lights from clusterBuffer (fog-pass pattern), IBL
                    irradiance/prefilter for sky+reflection fallback,
                    palette emission, Fresnel reflections
                    → blends into colorRT (load)
```

Key deltas vs. the OpenGL version, each a measured win or fidelity gain:

| Change | Why |
|---|---|
| Deferred voxel G-buffer; GI consumes it | removes the GI pass's duplicate primary DDA (the single biggest cost after primary itself) |
| Bitmask fine DDA in storage buffers | fewer memory fetches per step than per-voxel `texelFetch`; empty space skipped at 3 levels |
| Depth written by the primary pass | voxels get fogged, clouded, god-rayed, sun-flare-occluded by existing passes for free (original only composited against opaques) |
| One world structure instead of ≤4 sampler-bound volumes | GI is naturally global; no `MAX_VOLUMES` if-chains; unlimited volume count |
| Clustered lights (≤1024) replace 4 hard-coded point lights | fog pass already demonstrates the in-shader cluster loop |
| Engine IBL (irradiance/prefilter cubemaps) replaces the 2-color analytic sky in ambient, GI-miss, and reflection-miss | consistent with time-of-day / sky system; reflections get real environment |
| HDR-linear output into `colorRT` (RGBA16F) | original wrote gamma into an LDR-ish chain; Vapor's tonemapper + bloom handle exposure properly, emissive glow feeds bloom naturally |
| Blue-noise (hash fallback) sampling + engine GPU timing panel per pass | VISION's stochastic-effect skeleton; budgets verified pass-by-pass |

Feature parity checklist from the demo (all preserved): debug views 0–6,
GI/AO/shadow/point-light/reflections/denoiser toggles, raw|denoised split
compare with draggable divider, dig-with-E carving, multiple independent
volumes, per-voxel albedo hash variation, emissive glowstone GI.

### Backend notes (from the codebase audit)

- **Shaders are authored twice** (GLSL→SPIR-V at build via
  glslangValidator; MSL compiled at runtime) with mirrored bindings; the
  C++ pass code is written once against the RHI.
- Vulkan-RHI compute **cannot sample depth** — irrelevant here: GI/à-trous
  read the *voxel* G-buffer (our own storage/sampled images), not
  `depthStencilRT`. The two compute passes carry `PassFlags::RequiresCompute`.
- Fragment-depth write (`gl_FragDepth` / `[[depth(any)]]`) is new in this
  codebase but fully supported by the existing `RenderPassDesc.depthAttachment`
  + `PipelineDesc.depthWrite` plumbing; Main currently renders single-sampled
  (MSAA path disabled), so attaching `colorRT`/`depthStencilRT` with `load`
  is straightforward. NDC z is [0,1] — no GL `*0.5+0.5` remap; keep the
  `0.999999` clamp so hits stay in front of the sky.
- **Depth-convention caveat**: engine `Camera` uses `perspectiveZO`, but the
  ECS camera systems build `glm::perspective` ([-1,1]). The demo will set
  its projection via `perspectiveZO` explicitly, and the pass derives depth
  from the *active camera's* view-proj so both stay consistent. (A repo-wide
  cleanup to ZO is recommended but out of scope.)
- Per-frame uniform data uses the frame-slotted buffer helper; GPU-facing
  structs are vec4-packed and zero-initialized (ADR-009), dispatches are
  8×8 threadgroups (ADR-008).

## 6. Demo app — `Examples/MicroVoxel/`

Mirrors the Atmospheric example's shape on Vapor's idioms: one lean
`main.cpp` with its own `entt::registry`, `EngineCore` + `createRenderer`
(`--vulkan` flag supported), fly camera (`FlyCameraComponent` +
`CameraSystem` equivalent; WASD + R/F + IJKL look per engine defaults),
sun + `SkyComponent` time-of-day, and:

- one **large streaming volume** (the "bigger world" hero: 256×64×256 m
  target, 5 cm voxels, camera-radius streaming), plus two small
  high-detail dioramas at **2.5 cm** voxel size to show per-volume scale —
  proving multi-volume still works;
- dig-with-E via `VoxelVolumeSystem::raycast + carveSphere`;
- hotkey parity with the original demo (0–6, G/O/H/P/X/N/V/B, Esc);
- stats overlay via the existing ImGui debug panel + GPU timing panel.

CMake: `add_subdirectory(Examples)` under `PROJECT_IS_TOP_LEVEL` beside
`Vaporware`; target `microvoxel` links `Vapor`, reuses
`vapor_copy_engine_assets` / shader-compile targets.

## 7. Performance targets & measurement

Budgets at 1440p on Apple Silicon M3 (VISION's ≤6 ms whole-frame RT
budget as the yardstick), verified in the GPU timing panel:

| Pass | Budget |
|---|---|
| MicroVoxelPrimary (full res) | ≤ 2.0 ms with a 20× larger visible world than the GL demo |
| MicroVoxelGI + temporal (half res) | ≤ 1.5 ms |
| À-trous ×3 | ≤ 0.6 ms |
| Shade | ≤ 0.5 ms |

Also tracked: brick pool residency + upload bytes/frame (streaming), carve
latency (should be < 0.2 ms upload vs. the original's 16 MiB re-upload).
A Catch2 benchmark (`benchmarks/`) covers CPU-side generation throughput
(chunks/s) and carve/occupancy-rebuild cost.

## 8. Testing

Pure-logic Catch2 (`tests/test_voxel_world.cpp`), no GPU:

- generation determinism: fixed seed → stable content hash (native path);
- occupancy bitmasks and page-table entries consistent with materials
  after generation and after carving (incremental == full rebuild);
- `CarveSphere` edits exactly the voxels inside the sphere; `solidCount`
  bookkeeping; empty-brick reclamation returns slots to the free list;
- `RaycastVoxel` vs. brute-force reference on random rays;
- **CPU reference DDA** shared header (`voxel_dda.hpp`) exercised against
  brute-force marching — the same traversal logic the shaders mirror,
  restoring the "reference-validated DDA" property the Atmospheric comment
  describes.

## 9. Phases (each lands separately — ADR-009: one rendering change at a time)

1. **Phase 1 — faithful core on modern storage** *(the port proper)*
   `VoxelVolumeComponent`/`System`, `voxel_world` (chunks, page table,
   brick pool, bitmasks, CPU gen via enkiTS, carve/raycast), primary pass
   with depth write + forward shading (sun, shadow ray, corner AO, flat
   IBL-irradiance ambient, emission, reflections), both backends, demo app,
   tests. *Exit criteria: GL-demo content parity minus GI, ≤ 2 ms primary.*
2. **Phase 2 — GI pipeline** voxel G-buffer split (deferred shade), GI
   trace + temporal + à-trous compute passes, split-compare debug, GI-lit
   emissives. *Exit: side-by-side match with GL GI, ≤ 2.1 ms GI+denoise.*
3. **Phase 3 — the bigger world** streaming around the camera, LOD
   bricks, per-brick dirty uploads, eviction; clustered-light shading;
   large-world demo scene. *Exit: 256 m view distance at budget.*
4. **Phase 4 — stretch** voxel depth into PSSM cascades (voxels shadow
   the raster world), GPU compute generation, rough reflections with
   temporal accumulation, hardware-RT variant behind `RequiresRaytracing`.

## 10. Risks / open questions

- **Fragment-depth + early-Z**: writing `gl_FragDepth` disables early-Z
  for the primary pass; cost is bounded by box-raster coverage. If it
  bites, fall back to conservative box depth + per-pixel test.
- **Pool exhaustion**: streaming must degrade gracefully (skip farthest
  chunks first, log via stats panel) — never crash or corrupt slots.
- **Two shader dialects** double the maintenance of ~5 nontrivial
  shaders; mitigated by keeping DDA/shading structurally identical and
  documenting the binding contract in both files (repo convention).
- **Windows** is untested in CI (Vulkan path should work; not a goal).
- Native `Renderer_Metal` never gains the feature (accepted: it is the
  legacy A/B path).
