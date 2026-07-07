# Vapor GPU-Driven Rendering — Implementation Plan

Covers three feature tracks, in dependency order:

- **Track A — Vapor GPU Driven** (both backends): multi-draw indirect, bindless,
  GPU frustum culling, Hi-Z occlusion culling
- **Track B — Vapor Meshlet Culling** (Metal only): MeshOptimizer asset pipeline,
  mesh shading RHI API, meshlet buffers, task-shader per-meshlet culling,
  mesh-shader vertex generation
- **Track C — Vapor Cluster LOD** (Metal only): cluster LOD asset pipeline,
  per-cluster LOD selection in the task shader

This document is written to be executed phase by phase by an implementer with
no additional context. Every phase lists: data structures (exact layouts),
files to change, new methods (exact signatures), ordering constraints, and a
verification gate. **Do not start a phase before the previous phase's gate
passes.** Commit once per phase.

Ground rules (apply to every phase):

1. Never remove the existing (non-GPU-driven) path. Every new path is behind a
   renderer member toggle (listed per phase), default **off** until its gate
   passes, then flipped on by the user, not by the implementer.
2. Follow existing code conventions: handles (`BufferHandle` etc.) via the RHI,
   frame-slotted buffers for per-frame CPU-written data
   (`createFrameSlottedBuffer`), passes registered in the render graph in
   `Renderer::initialize` (renderer.cpp ~line 755+), capability gating via
   `PassFlags` / `RHICapabilities`, ImGui toggles in `drawGraphicsImGui`.
3. MSL structs: `float3` occupies 16 bytes. Every struct shared with a shader
   gets an explicit padded C++ layout + a `static_assert(sizeof(...))`, same
   as `render_data.hpp` does today. This has bitten this codebase repeatedly.
4. Verification on Linux is lavapipe (software) via
   `xvfb-run -a env VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation ./build-linux/Vaporware/main`
   — correctness/validation only, never timing. Metal phases can only be
   compile-checked off-device; flag them for on-device testing by the user.
5. GPU timings panel ("GPU Pass Timings") is the perf gate instrument: new
   passes must be named so they appear there.

Key existing anchors (verified against the tree):

| Anchor | Location |
|---|---|
| `RHI::draw / drawIndexed` (no indirect yet) | `Vapor/include/Vapor/rhi.hpp:512` |
| `BufferUsage` enum (no Indirect yet) | `Vapor/include/Vapor/rhi.hpp:84` |
| `Renderer::registerMesh` (per-mesh VB/IB + BLAS) | `Vapor/src/renderer.cpp:456` |
| CPU culling (`performCulling`) + `visibleDrawables` | `Vapor/src/renderer.cpp` (`render()` ~728) |
| `CameraRenderData.frustumPlanes[6]` already exists | `Vapor/include/Vapor/render_data.hpp` |
| Instance data: `InstanceData` array, `drawableToInstanceID` | renderer.cpp `updateBuffers()` |
| Metal mesh shading available in vendored metal-cpp | `MTL::MeshRenderPipelineDescriptor` (`MTLRenderPipeline.hpp:521`), `objectFunction`/`meshFunction` |
| vcpkg manifest | `/vcpkg.json` (add `"meshoptimizer"`) |
| Prepass depth (Hi-Z source) | `depthStencilRT`, written by `prePass()` |

---

## Track A — GPU Driven (both backends)

### Phase A1 — RHI indirect draw API

**Goal:** `drawIndexedIndirect` on both backends. No behavior change anywhere
yet (API only + unit smoke).

**Data structures** — add to `Vapor/include/Vapor/rhi.hpp`:

```cpp
// Layout is IDENTICAL to VkDrawIndexedIndirectCommand and to Metal's
// MTLDrawIndexedPrimitivesIndirectArguments (both are 5 x uint32):
//   VK:  indexCount, instanceCount, firstIndex, vertexOffset, firstInstance
//   MTL: indexCount, instanceCount, indexStart, baseVertex,  baseInstance
// so one struct serves both backends with no translation.
struct DrawIndexedIndirectCommand {
    Uint32 indexCount;
    Uint32 instanceCount;   // culling writes 0 (skip) or N (draw)
    Uint32 firstIndex;
    int32_t vertexOffset;
    Uint32 firstInstance;
};
static_assert(sizeof(DrawIndexedIndirectCommand) == 20);
```

**Changes:**

1. `rhi.hpp`:
   - `BufferUsage`: add `Indirect` enumerator.
   - `class RHI`: add
     `virtual void drawIndexedIndirect(BufferHandle argBuffer, size_t offset, Uint32 drawCount, Uint32 stride = sizeof(DrawIndexedIndirectCommand)) = 0;`
   - `RHICapabilities`: add `bool multiDrawIndirect = false;`
2. `rhi_vulkan.cpp`:
   - `convertBufferUsage`: `Indirect` → `VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT`
     (storage: the cull kernel writes it; transfer: CPU seeds it).
   - `drawIndexedIndirect`: enable device feature `multiDrawIndirect` at device
     creation (check `VkPhysicalDeviceFeatures.multiDrawIndirect`; MoltenVK
     exposes it). If the feature is available call
     `vkCmdDrawIndexedIndirect(cmd, buf, offset, drawCount, stride)` once;
     otherwise loop `drawCount` times with one `vkCmdDrawIndexedIndirect(...,1,...)`
     advancing `offset += stride` (portable fallback, still GPU-sourced args).
   - Set `capabilities.multiDrawIndirect = true` when the feature bit is on.
   - Barrier note: the cull dispatch → indirect read hazard is
     `VK_ACCESS_SHADER_WRITE_BIT → VK_ACCESS_INDIRECT_COMMAND_READ_BIT`,
     stages `COMPUTE → DRAW_INDIRECT`. Extend `computeBarrier()` (or add
     `indirectBarrier()`) to cover this; lavapipe validation will catch it if
     missed.
3. `rhi_metal.cpp`:
   - Metal has no native multi-draw: implement as a loop of
     `renderEncoder->drawIndexedPrimitives(primitiveType, indexType, indexBuffer,
     indexBufferOffset, argBuffer, argOffset + i * stride)` — the 5-word arg
     block is read by the GPU per draw. `capabilities.multiDrawIndirect = true`
     (semantics satisfied; cost is one encoder call per slot, args still
     GPU-authored — that is the part culling needs).
   - IMPORTANT: the indexed-indirect overload takes the INDEX BUFFER at encode
     time; `firstIndex/vertexOffset` come from the arg block. Bind the pooled
     index buffer (Phase A2) before calling.

**Gate A1:** builds on Linux; a temporary debug path (behind
`VAPOR_TEST_INDIRECT=1`) draws one known mesh via a CPU-filled
`DrawIndexedIndirectCommand` buffer and looks identical to the direct path on
lavapipe. Remove the env path after checking. Vulkan validation clean.

---

### Phase A2 — Global geometry pool

**Goal:** all static mesh geometry in ONE vertex buffer + ONE index buffer, so
a single indirect buffer can source every draw. Per-mesh buffers stay (BLAS
and the legacy path keep using them) — memory cost is accepted for v1.

**Data structures** — add to `Vapor/include/Vapor/renderer.hpp`:

```cpp
struct MeshPoolRange {          // per MeshId, parallel to `meshes`
    Uint32 firstIndex   = 0;    // into pooled index buffer
    int32_t vertexOffset = 0;   // into pooled vertex buffer (in vertices)
    Uint32 indexCount   = 0;
    glm::vec3 boundsCenter{0}; // object-space bounding sphere (for culling)
    float boundsRadius  = 0;
};
BufferHandle poolVertexBuffer;  // grows by re-create+copy (see below)
BufferHandle poolIndexBuffer;
std::vector<MeshPoolRange> meshPool;   // index == MeshId
```

**Changes (renderer.cpp):**

1. `registerMesh`: after the existing per-mesh buffer creation, append the same
   vertices/indices into CPU-side staging vectors
   (`std::vector<VertexData> poolVertsCPU; std::vector<Uint32> poolIndicesCPU;`)
   and record the `MeshPoolRange` (firstIndex = poolIndicesCPU.size() before
   append, vertexOffset = poolVertsCPU.size() before append). Compute the
   object-space bounding sphere here (center = AABB center, radius =
   max distance to it) — reuse whatever bounds `performCulling` uses if the
   mesh already carries them.
2. Add `void Renderer::uploadMeshPool()` — creates/recreates
   `poolVertexBuffer/poolIndexBuffer` (usage Vertex|Index respectively,
   `MemoryUsage::GPU`) sized to the CPU vectors and uploads once. Call it at
   the end of `stage()` (idempotent: skip if sizes unchanged).
3. Do NOT touch BLAS creation in this phase.

**Gate A2:** temporary debug toggle draws the whole `visibleDrawables` list
from the pool via `drawIndexedIndirect` with a CPU-filled command buffer
(instanceCount=1 per visible drawable, `firstInstance` = the drawable's
existing instance ID so the instance-data indexing keeps working — see note
below) and matches the direct path pixel-for-pixel on lavapipe.

> **Instance-ID note (critical):** the current shaders read the instance index
> from a push/`setVertexBytes` value, NOT from `gl_InstanceIndex`. For the
> indirect path the shader must switch to
> `gl_InstanceIndex` (GLSL) / `instance_id + base_instance` (MSL) so that
> `firstInstance` carries the per-draw instance ID with zero CPU involvement.
> Add this as a shader variant: `RHIMainIndirect.vert` (GLSL) and a
> `USE_INSTANCE_ID` function-constant path in the MSL vertex shader. Vulkan
> requires `VkPhysicalDeviceVulkan11Features::shaderDrawParameters` or the
> SPIR-V `DrawParameters` capability for `gl_BaseInstance`-relative semantics —
> `gl_InstanceIndex` already includes `firstInstance` in Vulkan, so plain
> `gl_InstanceIndex` is enough there. In MSL, declare
> `uint iid [[instance_id]], uint base [[base_instance]]` and use `iid` (Metal's
> instance_id also starts at baseInstance? It does NOT — add them: `iid + 0`
> where iid starts at base_instance... **verify on device**: MSL `instance_id`
> begins at `baseInstance` per Metal docs; if artefacts appear, use
> `[[base_instance]]` explicitly and add).

---

### Phase A3 — GPU frustum culling

**Goal:** replace the CPU `performCulling` + per-draw loop for the MAIN and
PRE passes with: CPU uploads N draw slots (one per submitted drawable, ordered
by material batch), a compute pass zeroes/keeps `instanceCount`, one
`drawIndexedIndirect` per material batch.

**Toggle:** `bool gpuDrivenEnabled = false;` (panel checkbox "GPU driven
culling/draw", Graphics section).

**Data structures** (shared GPU structs → new header
`Vapor/include/Vapor/gpu_driven_data.hpp`, padded + static_asserted):

```cpp
struct alignas(16) CullInstanceData {   // one per draw slot, CPU-written
    glm::vec4 sphere;      // xyz = world-space center, w = radius
    Uint32 drawSlot;       // index into the indirect command buffer
    Uint32 _pad[3];
};
struct alignas(16) CullParams {
    glm::vec4 frustumPlanes[6];  // world-space, normalized, from currentCamera
    Uint32 drawCount;
    Uint32 hiZEnabled;           // 0 until Phase A5
    glm::vec2 hiZSize;
    glm::mat4 viewProj;          // for Hi-Z screen-rect projection (A5)
    float nearPlane; float _pad[3];
};
```

Renderer members: `BufferHandle indirectCmdBuffer;` (usage `Indirect`, GPU),
`BufferHandle cullInstanceBuffer;` (Storage, CPUtoGPU, frame-slotted),
`BufferHandle cullParamsBuffer;` (Uniform→use Storage on Vulkan set-1
convention, frame-slotted), `ComputePipelineHandle frustumCullPipeline;`
(+ Vulkan `PipelineHandle` NOT needed — culling is compute on BOTH backends;
lavapipe supports compute, and no depth texture is read in this phase).

**Shaders:**

- `Vapor/assets/shaders/FrustumCull.comp` (GLSL, compiled to .spv with
  glslangValidator like the others): local_size_x=64. `id >= drawCount →
  return`. Sphere-vs-6-planes: `dot(plane.xyz, c) + plane.w < -r → culled`.
  Writes `cmds[slot].instanceCount = culled ? 0 : 1`.
  Bindings (Vulkan compute convention — SSBO set 0 at binding N via
  `setComputeBuffer(N)`): b0 = CullParams, b1 = CullInstanceData[],
  b2 = DrawIndexedIndirectCommand[] (read-write).
- `Vapor/assets/shaders/3d_frustum_cull.metal` — same logic, kernel
  `computeMain`, buffers 0/1/2 identical order.

**CPU seeding (renderer.cpp, new `void buildIndirectDraws()`, called from
`render()` when `gpuDrivenEnabled`, replacing `performCulling`'s visibility
narrowing but KEEPING `updateBuffers()` semantics):**

1. Iterate `frameDrawables` grouped by material (same `std::map<MaterialId,...>`
   ordering the main pass uses today) — assign each drawable a draw slot and an
   instance ID in that order; fill `InstanceData` (existing) and
   `CullInstanceData` (sphere = `transform * boundsCenter`, radius scaled by
   max column scale of the transform) and the initial
   `DrawIndexedIndirectCommand` (from `meshPool[mesh]`, instanceCount=1,
   firstInstance = instance ID).
2. Upload commands with `rhi->updateBuffer(indirectCmdBuffer, ...)` (transfer
   dst usage is already on Indirect buffers per A1).
3. Record per-batch `(materialId, firstSlot, slotCount)` into
   `std::vector<IndirectBatch> indirectBatches;`.

**Graph:** new pass `"FrustumCull"` (flags `RequiresCompute`) registered
immediately BEFORE `"PrePass"`; executes the dispatch
(`(drawCount+63)/64`), then the indirect barrier (A1).

**Draw loop change (mainRenderPass + prePass):** when `gpuDrivenEnabled`,
replace the inner per-drawable loop with, per `IndirectBatch`:
`bindMaterial(batch.materialId)`; bind pooled vertex buffer at the mesh slot
(binding 3 Metal / binding 0 Vulkan — same slots as today's per-mesh binds);
`rhi->bindIndexBuffer(poolIndexBuffer, 0)`;
`rhi->drawIndexedIndirect(indirectCmdBuffer, batch.firstSlot * sizeof(cmd), batch.slotCount)`.
Use the instance-ID shader variants from A2's note.

**Gate A3:** on lavapipe, `gpuDrivenEnabled=1` renders identically to the
legacy path (toggle back and forth in the panel); validation clean; culling
correctness spot-check = walk the camera so objects leave the frustum, frame
time drops in the timings panel, no popping at frustum edges. THEN user
verifies on Metal.

---

### Phase A4 — Bindless materials

**Goal:** remove per-batch `bindMaterial` so ONE indirect call can cover ALL
materials. This is the step that makes A3 truly multi-draw.

**Design decision:** bindless applies to the GPU-driven path only. Legacy path
keeps per-draw binding forever (fallback).

**RHI API (`rhi.hpp`):**

```cpp
virtual Uint32 registerBindlessTexture(TextureHandle tex, SamplerHandle smp) = 0; // stable index
virtual void   bindBindlessTable(Uint32 set /*Vulkan set index; ignored on Metal*/) = 0;
RHICapabilities: bool bindless = false;
```

- **Vulkan** (`rhi_vulkan.cpp`): descriptor-indexing set. Device creation adds
  `VkPhysicalDeviceDescriptorIndexingFeatures` (runtimeDescriptorArray,
  shaderSampledImageArrayNonUniformIndexing, descriptorBindingPartiallyBound,
  descriptorBindingUpdateAfterBind — all supported by MoltenVK and lavapipe).
  One global descriptor set (set 3 — sets 0..2 are taken by the existing
  convention) with a single binding 0: `sampler2D textures[]` (count = 4096,
  VARIABLE_DESCRIPTOR_COUNT + PARTIALLY_BOUND + UPDATE_AFTER_BIND flags).
  `registerBindlessTexture` writes the next free element and returns its index.
  `bindBindlessTable` binds set 3 to the graphics pipeline layout (extend
  `globalPipelineLayout` creation with the 4th set layout).
- **Metal** (`rhi_metal.cpp`): one argument buffer containing a
  `MTL::Texture*` table: allocate an `MTL::Buffer` of
  `4096 * sizeof(MTL::ResourceID)` if targeting Metal 3 bindless
  (`texture2d<float> tbl [[buffer(N)]]` via `descriptor_set`… **simplest
  robust scheme**: MSL `array<texture2d<float>, 4096>` argument buffer written
  with `MTL::ArgumentEncoder`). Every registered texture also needs residency:
  keep a `std::vector<MTL::Resource*>` and call
  `renderEncoder->useResources(list.data(), list.size(), MTL::ResourceUsageRead)`
  in `bindBindlessTable`.

**Material GPU table:** extend the existing `MaterialData` upload (or add a
parallel `MaterialBindless { Uint32 albedoIdx, normalIdx, ormIdx, emissiveIdx; }`
SSBO, index = materialId). `getOrCreateTexture` also calls
`registerBindlessTexture` and stores the returned index in `RenderTexture`.

**Shader variants:** `RHIMainBindless.frag` (GLSL:
`#extension GL_EXT_nonuniform_qualifier; layout(set=3,binding=0) uniform sampler2D texTable[];`
sample via `texTable[nonuniformEXT(mat.albedoIdx)]`) and an MSL function-constant
variant reading the argument-buffer table. Material index reaches the fragment
stage through the existing per-instance `InstanceData` (it already carries a
material index — verify; if not, add `Uint32 materialIndex` to it).

**Draw loop:** `indirectBatches` collapses to ONE batch; the per-batch
`bindMaterial` disappears on the GPU-driven path.

**Gate A4:** lavapipe pixel-match vs A3, validation clean (watch for
update-after-bind pool flags), then Metal on-device (argument-buffer residency
is the risk area — missing useResources = GPU fault, which
`VAPOR_METAL_DEBUG=1` will attribute).

---

### Phase A5 — Hi-Z occlusion culling

**Goal:** cull draw slots whose bounding sphere is provably behind the depth
buffer. v1 uses the SAME-FRAME prepass depth: PrePass still renders everything
(it is cheap, depth+MRT only), Hi-Z is built from it, and the cull pass
(re-run AFTER Hi-Z, BEFORE Main) trims the expensive Main-pass PBR work.

**New resources:** `TextureHandle hiZPyramid;` — R32F (NOT the depth format;
copy resolves sampling headaches), full mip chain, half-res base
(`(w+1)/2 × (h+1)/2`), usage Storage|Sampled. `ComputePipelineHandle hiZBuildPipeline;`
Panel toggle `bool hiZCullingEnabled = false;`.

**Passes (graph order):** `PrePass` → `"HiZBuild"` (RequiresCompute) →
`"OcclusionCull"` (RequiresCompute; a SECOND dispatch of the same cull kernel
with `hiZEnabled=1`) → `Main`.
The A3 frustum pass stays before PrePass (it also serves PrePass); the
occlusion pass only rewrites `instanceCount` for slots that survived frustum.

**HiZBuild kernel** (`HiZBuild.comp` + `3d_hiz_build.metal`): mip 0 = MAX of
the 2×2 depth texels it covers (standard depth, far=1 ⇒ MAX is conservative
for occlusion: stored value ≥ every real depth in the footprint). Each
subsequent mip = MAX of 2×2 of the previous, dispatched per-mip with
`computeBarrier()` between mips (loop in the renderer, one dispatch per mip,
same pattern as `bloomDownsamplePass`). Vulkan cannot read the depth texture
in compute (established constraint) — do mip 0 as a FRAGMENT pass into the
R32F base (like Velocity.frag does), then compute for mips 1..N which read the
R32F pyramid itself (color format: storage-image legal). Metal reads depth
directly in the mip-0 kernel.

**Cull test (added to the A3 kernel, `hiZEnabled` branch):** project the
sphere to a screen-space AABB with `viewProj` (guard: if the sphere crosses
the near plane, KEEP the draw — never cull). Pick
`mip = ceil(log2(max(rectWidthPx, rectHeightPx)))` clamped to the pyramid
range, take the 4 pyramid texels covering the rect at that mip, and cull iff
`minNdcDepthOfSphere > max(4 texels)`. `minNdcDepthOfSphere` = project the
sphere point nearest the camera. Conservative by construction (MAX pyramid +
4-texel cover + near-plane keep).

**Gate A5:** lavapipe — camera in front of a large wall with objects behind
it: those objects' draws disappear (add a debug counter: cull kernel also
atomically increments a `Uint32 stats[2]` buffer read back next frame and
shown in the panel: "frustum culled / occlusion culled"). No popping when
strafing (the 1-frame-same-frame design has no temporal artefacts by
construction). Then Metal on-device.

---

## Track B — Meshlet Culling (Metal only)

> Requires A1–A3 landed (pool + GPU-driven mindset), A4/A5 optional but
> recommended first. All of Track B is `#ifdef __APPLE__`-adjacent at the RHI
> level and gated on a new `RHICapabilities::meshShading` (Metal: true on
> Apple7+/M-series — query `device->supportsFamily(MTL::GPUFamilyApple7)`;
> Vulkan backend reports false, all meshlet passes are graph-gated off).

### Phase B1 — MeshOptimizer asset pipeline + meshlet buffers

**Dependency:** add `"meshoptimizer"` to `vcpkg.json` dependencies; CMake:
`find_package(meshoptimizer CONFIG REQUIRED)` +
`target_link_libraries(Vapor PRIVATE meshoptimizer::meshoptimizer)` in
`Vapor/CMakeLists.txt`.

**Constants:** `MESHLET_MAX_VERTS = 64`, `MESHLET_MAX_TRIS = 124`
(meshoptimizer's recommended NV/Apple-friendly sizes), cone weight `0.5f`.

**Data structures (`gpu_driven_data.hpp`):**

```cpp
struct alignas(16) MeshletDesc {
    glm::vec3 center;  float radius;          // meshlet bounding sphere (object space)
    glm::vec3 coneApex; float _pad0;          // backface cone (object space)
    glm::vec3 coneAxis; float coneCutoff;     // cutoff = cos(angle/2), from meshopt
    Uint32 vertexOffset;    // into meshletVertices (uint indices into pool VB)
    Uint32 triangleOffset;  // into meshletTriangles (u8 micro-indices), byte offset
    Uint32 vertexCount;
    Uint32 triangleCount;
    // Track C adds: float selfError, parentError; Uint32 lodLevel, _pad;
};
struct MeshletRange {   // per MeshId — extend MeshPoolRange or parallel vector
    Uint32 firstMeshlet = 0;
    Uint32 meshletCount = 0;
};
```

Renderer members: `BufferHandle meshletBuffer, meshletVertexBuffer,
meshletTriangleBuffer;` (all Storage/GPU), `std::vector<MeshletRange>
meshMeshlets;`, CPU staging vectors mirroring the pool pattern of A2.

**Build (in `registerMesh`, after the pool append):**

```cpp
size_t maxMeshlets = meshopt_buildMeshletsBound(indices.size(), MESHLET_MAX_VERTS, MESHLET_MAX_TRIS);
std::vector<meshopt_Meshlet> local(maxMeshlets);
std::vector<unsigned int> mv(maxMeshlets * MESHLET_MAX_VERTS);
std::vector<unsigned char> mt(maxMeshlets * MESHLET_MAX_TRIS * 3);
size_t count = meshopt_buildMeshlets(local.data(), mv.data(), mt.data(),
    indices.data(), indices.size(), &vertices[0].position.x, vertices.size(),
    sizeof(VertexData), MESHLET_MAX_VERTS, MESHLET_MAX_TRIS, 0.5f);
// per meshlet: meshopt_computeMeshletBounds(...) -> center/radius/coneApex/coneAxis/coneCutoff
// remap: meshlet-local vertex indices reference THIS mesh's pool range:
//   stored vertex index = poolRange.vertexOffset + mv[m.vertex_offset + i]
```

Upload in `uploadMeshPool()` alongside the pool buffers. This is runtime
build (v1); offline caching through `asset_serializer.cpp` (cereal) is a
follow-up marked **B1.5 (optional)** — serialize the four arrays keyed by a
hash of (vertices,indices,constants).

**Gate B1:** log per-scene totals (meshlets, avg tris/meshlet ≈ 60–124);
builds on both platforms (meshoptimizer is pure CPU, so Linux builds it too);
no rendering change yet.

### Phase B2 — RHI mesh-shading API (Metal backend)

**`rhi.hpp`:**

```cpp
struct MeshPipelineDesc {
    ShaderHandle objectShader;   // task
    ShaderHandle meshShader;
    ShaderHandle fragmentShader;
    // reuse: blendMode, depth*, cullMode, colorAttachmentFormats,
    // depthAttachmentFormat, sampleCount — same fields as PipelineDesc
    Uint32 payloadSize = 0;      // object->mesh payload bytes (Metal needs it)
    Uint32 maxMeshThreadgroupsPerObject = 0;
};
virtual PipelineHandle createMeshPipeline(const MeshPipelineDesc&) { return {}; }
virtual void drawMeshThreadgroups(Uint32 gx, Uint32 gy, Uint32 gz,
                                  Uint32 objectThreads, Uint32 meshThreads) {}
virtual void setObjectBuffer(Uint32 binding, BufferHandle, size_t offset, size_t size) {}
virtual void setObjectBytes(const void* data, size_t size, Uint32 binding) {}
virtual void setMeshBuffer(Uint32 binding, BufferHandle, size_t offset, size_t size) {}
RHICapabilities: bool meshShading = false;
```

(Default no-op bodies so the Vulkan backend needs no changes.)

**`rhi_metal.cpp`:** `createMeshPipeline` uses
`MTL::MeshRenderPipelineDescriptor` (`setObjectFunction/setMeshFunction/
setFragmentFunction`, `setPayloadMemoryLength(desc.payloadSize)`,
`setMaxTotalThreadsPerObjectThreadgroup/…PerMeshThreadgroup`, color/depth
formats same mapping as the existing pipeline path) +
`device->newRenderPipelineState(desc, MTL::PipelineOptionNone, nullptr, &err)`
(the MeshRenderPipelineDescriptor overload). `drawMeshThreadgroups` →
`renderEncoder->drawMeshThreadgroups(MTL::Size(gx,gy,gz),
MTL::Size(objectThreads,1,1), MTL::Size(meshThreads,1,1))`.
`setObjectBuffer/Bytes` → `renderEncoder->setObjectBuffer/setObjectBytes`;
`setMeshBuffer` → `setMeshBuffer`. Capability: `capabilities.meshShading =
device->supportsFamily(MTL::GPUFamilyApple7);` (verify symbol exists in
vendored metal-cpp; if the enum is missing use `MTL::GPUFamily(1007)`).

**Gate B2:** compile-only on Linux (all Metal code `#ifdef __APPLE__`); a
trivial on-device smoke (fullscreen triangle emitted from a 1-meshlet mesh
shader) is written but executed by the user.

### Phase B3 — Task/mesh shaders: per-meshlet culling + vertex generation

**New pass:** `"MeshletMain"` — an ALTERNATIVE main pass, graph-registered
right after `"Main"`, both internally guarded:
`mainRenderPass()` returns early when `meshletPathEnabled`, `meshletMainPass()`
returns early when `!meshletPathEnabled || !capabilities.meshShading`.
Panel checkbox "Meshlet path (mesh shading)".

**Shader file:** `Vapor/assets/shaders/3d_meshlet_main.metal` with three
functions:

- `objectMain` (`[[object]]`): one threadgroup per 32 meshlets of one
  drawable-instance (dispatch: `gx = ceil(meshletCount/32)`, `gy = 1` per
  draw; v1 dispatches one `drawMeshThreadgroups` per visible drawable from the
  CPU loop — GPU-driven fusion with A3's indirect comes later, Metal's
  `drawMeshThreadgroupsWithIndirectBuffer` exists for it and is marked B3.5).
  Each thread tests ONE meshlet:
  1. frustum: sphere (transformed by the instance matrix from
     `InstanceData`) vs `CullParams.frustumPlanes` — same math as A3;
  2. backface cone: `dot(normalize(coneApex_world - cameraPos), coneAxis_world)
     >= coneCutoff → cull` (meshopt's documented test; skip when
     `coneCutoff >= 1`, i.e. degenerate);
  3. (after A5) Hi-Z: same rect/mip test, `hiZPyramid` bound as object-stage
     texture.
  Survivors: `payload.meshletIndices[wavePrefixSum...]`; end with
  `mesh_grid_properties.set_threadgroups_per_grid(uint3(survivorCount,1,1))`.
- `meshMain` (`[[mesh]]`): threadgroup = one surviving meshlet;
  `metal::mesh<VOut, PrimOut, MESHLET_MAX_VERTS, MESHLET_MAX_TRIS,
  metal::topology::triangle>`. Threads 0..vertexCount-1: fetch
  `poolVB[meshletVertices[m.vertexOffset + t]]`, run the SAME vertex transform
  as the main vertex shader (world = instance matrix, clip = viewProj) and
  `mesh.set_vertex(t, v)`. Threads 0..triangleCount-1:
  `mesh.set_index(t*3+k, meshletTriangles[m.triangleOffset + t*3 + k])`;
  `set_primitive_count(triangleCount)`.
- `fragmentMain`: v1 = the depth/albedo PREPASS fragment (small, proves the
  pipeline); v2 = the full PBR fragment (copy the existing one; bindings move
  to mesh-stage-visible buffer indices — keep the same buffer TABLE as the
  main pass so `bindMaterial` still works, but note Metal object/mesh/fragment
  stages each have separate binding tables: fragment bindings are untouched).

**Buffers (object stage):** b0 CullParams (frame-slotted), b1 MeshletDesc[],
b2 InstanceData[], b3 per-draw `{Uint32 instanceID, meshletFirst, meshletCount}`
via `setObjectBytes`. **Mesh stage:** b0 pool VB, b1 meshletVertices,
b2 meshletTriangles, b3 MeshletDesc[], b4 camera, b5 instance data,
b6 the same per-draw bytes.

**Gate B3:** on-device (user): meshlet path on/off renders the same scene
(depth-prepass-level first, then PBR); GPU timings show `MeshletMain`;
frustum/cone culling verified with the stats counter pattern from A5
(object stage atomically bumps counters in a debug buffer).

---

## Track C — Cluster LOD (Metal only)

> Requires B1–B3. This is discrete per-cluster LOD (each meshlet knows which
> LOD level it belongs to and its geometric error), NOT a full Nanite DAG:
> LOD chains are built per mesh with locked borders so adjacent clusters from
> the same level never crack; selection granularity is per meshlet-GROUP with
> a self/parent error pair, which gives crack-free mixed-LOD selection under
> the standard "self ≤ τ < parent" rule.

### Phase C1 — LOD chain asset pipeline

**Build (extends the B1 code in `registerMesh` / a helper
`buildMeshletLODs(vertices, indices)`):**

1. `LOD_LEVELS = 4`. `lodIndices[0] = indices`. For L = 1..3:
   `meshopt_simplify(lodIndices[L], lodIndices[L-1], ..., target =
   lodIndices[L-1].size()/2, target_error = FLT_MAX, options =
   meshopt_SimplifyLockBorder, &lodError[L])` — locked borders are what
   prevents cracks between neighboring clusters at the same level.
   Record `lodError[L]` scaled to object units
   (`meshopt_simplifyScale(...) * raw_error`, accumulated:
   `err[L] = max(err[L-1], scale*raw)` so error grows monotonically up the
   chain).
2. Build meshlets per LOD level (B1 code), appending into the same global
   meshlet buffers; each `MeshletDesc` gets `lodLevel = L`,
   `selfError = err[L]`, `parentError = err[L+1]` (`FLT_MAX` for the coarsest
   level so it is always selectable as last resort).
3. `MeshletRange` becomes per-mesh-per-LOD:
   `Uint32 firstMeshlet[LOD_LEVELS], meshletCount[LOD_LEVELS];` and a combined
   full range (the task shader iterates the FULL range and lets the error test
   pick — simplest correct scheme, no CPU LOD decision at all).

**`MeshletDesc` additions** (reserved in B1): `selfError, parentError,
lodLevel`.

**Gate C1:** log per-mesh tri counts per level (≈ 100/50/25/12.5%); total
meshlet count grows ~2x (sum of the geometric series); still renders LOD 0
only (selection lands in C2) — set `parentError` test off (`hiZEnabled`-style
flag `lodSelectEnabled=0` forces `lodLevel==0` selection) so B3 output is
unchanged.

### Phase C2 — Per-cluster LOD selection in the task shader

**CullParams additions:** `float lodErrorThresholdPx = 1.0f;` (panel slider
0.25..8 px), `float projScale;` (CPU: `0.5 * screenHeightPx * proj[1][1]` —
the standard perspective screen-space-error factor), `Uint32 lodSelectEnabled;`

**Task-shader selection (replaces the `lodLevel==0` filter):** for meshlet m
with world-space sphere center c, radius r (instance-transformed):

```
dist   = max(length(c - cameraPos) - r, znear)
errPx(e) = e * projScale / dist
selected = errPx(selfError) <= threshold && errPx(parentError) > threshold
```

Coarsest level's `parentError = FLT_MAX` ⇒ always passes the right half;
finest level's `selfError = 0` ⇒ always passes the left half — exactly one
level satisfies both for any distance, so coverage is complete and
non-overlapping per group. Then the surviving meshlet continues through the
B3 frustum/cone/Hi-Z tests unchanged.

**Gate C2 (on-device, user):** slider sweep visibly swaps density with no
cracks and no double-shading (enable a debug color mode: task shader writes
`lodLevel` into the payload, mesh shader passes it to the fragment as a flat
color — panel toggle "Visualize cluster LOD"); GPU timings for `MeshletMain`
drop as the camera pulls back; triangle-count stat (atomic counter) tracks
distance.

---

## Suggested commit sequence (one commit per line)

1. A1 RHI indirect API + backends + smoke
2. A2 geometry pool + instance-ID shader variants
3. A3 frustum-cull compute + indirect draw path + panel toggle
4. A4 bindless (Vulkan descriptor indexing + Metal argument buffer) + single-batch MDI
5. A5 Hi-Z build + occlusion phase + stats counters
6. B1 meshoptimizer + meshlet build/upload (+ B1.5 cache, optional)
7. B2 mesh-shading RHI API (Metal) + on-device smoke handed to user
8. B3 task/mesh/fragment meshlet path + panel toggle (+ B3.5 indirect fusion, optional)
9. C1 LOD chain build (selection dormant)
10. C2 per-cluster LOD selection + threshold slider + LOD visualizer

## Known risks / explicit non-goals

- **MoltenVK**: multiDrawIndirect and descriptor indexing are supported;
  `drawIndirectCount` is NOT assumed anywhere in this plan (zero-instanceCount
  slots instead of compaction keeps us off that feature).
- **Metal instance_id vs base_instance** (A2 note) must be verified on device
  before A3 is signed off.
- **BLAS + pool**: BLASes keep using per-mesh buffers; unifying them onto the
  pool (extending `AccelStructGeometry` with buffer offsets) is an
  optimization follow-up, not part of these tracks.
- **No Nanite DAG**: Track C is discrete per-cluster LOD with locked-border
  simplification. Hierarchical cluster groups/DAG streaming is out of scope.
- **Skinned/animated meshes**: excluded from the pool and meshlet paths; they
  stay on the legacy per-mesh path (guard: only pool meshes registered via
  `registerMesh` with no skinning data — currently all of them).
- **Water/DOF passes** (not yet on RHI) are unaffected; if they land later
  they use the legacy path until explicitly migrated.
