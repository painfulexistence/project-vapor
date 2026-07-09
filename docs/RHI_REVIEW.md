# RHI Review

Capstone review of the `Renderer → RHI → {rhi_metal, rhi_vulkan}` abstraction at
the close of the RHI split. Synthesized from three focused passes (interface +
renderer usage, Vulkan backend, Metal backend). Line numbers are indicative and
may drift.

## Executive summary

The RHI has a **clean resource/frame model** and a **leaky binding model**.

"What resources exist and when they live" is well handled: typed handles,
descriptor structs, disciplined create/destroy pairing, deferred destruction,
frame-partitioned staging, and a genuinely nice capability-gated `RenderGraph` /
`PassFlags` / `RHICapabilities` mechanism for skipping unsupported passes.

"How a shader consumes those resources" has escaped into the consumer. The
renderer hand-reconciles Metal buffer-table indices against Vulkan
`(set, binding)` *and* raw push-constant byte offsets, per shader, because
`PipelineDesc` carries no descriptor-set-layout / reflection info (`rhi.hpp:305`
`// TODO: Add descriptor set layouts`). The visible symptoms: a dead
`(set, binding)` API, ~40 `if (backend == …)` branches, a fragile per-shader
"FULL contract," and an outright `dynamic_cast` to `RHI_Metal` for ImGui.

Both backends are individually mature and defensively written. The correctness
risks that remain are concentrated in a few specific, mostly off-happy-path
places, plus one cross-cutting portability landmine (`Depth24Stencil8`) that both
backend reviews independently flagged.

## Architecture: the binding-model leak

The single highest-leverage issue. Pipelines have no layout/reflection data, so
binding is reconciled by hand in `renderer.cpp`:

- **Dead descriptor API.** `setUniformBuffer` / `setStorageBuffer` (the
  `(set, binding)` model the interface advertises) are never called; all binding
  goes through raw-index `setVertexBuffer` / `setFragmentBuffer`, and `setTexture`
  is always `set = 0`. The interface presents a model the consumer bypasses.
- **ABI knowledge in the consumer.** The main pass encodes Vulkan push-constant
  byte offsets ("binding 11 → push offset 112", "push offset 96 (binding 2)") and
  per-backend buffer-table indices — shader ABI that belongs behind the RHI.
- **The "FULL contract."** Correctness depends on a hand-maintained table where
  the same slot means different things per backend (Metal `fragment buffer(2)` =
  clusters vs Vulkan set1 b2 = PSSM). A comment records that a past mismatch
  caused a GPU hang. Nothing in the RHI enforces it; a shader edit silently
  corrupts bindings.
- **~40 `backend ==` branches** gate binding order, many per-frame. They are
  "justified" only because the RHI can't express a shader's per-backend binding
  map.
- **`dynamic_cast<RHI_Metal*>` for ImGui** (`renderer.cpp:~769,~1011`): the
  backend-agnostic renderer builds `MTL::RenderPassDescriptor` inline. For ImGui
  the abstraction is fully bypassed.

**Recommendation:** attach a reflection-driven binding layout to pipelines so the
renderer binds by logical name/slot once and each backend maps it. This collapses
most branching, retires the dead API, and makes the main-pass contract
machine-checked instead of comment-enforced. This is a large, standalone piece of
work — the natural "RHI v2" follow-up.

## Findings by severity

### High

| # | Area | Finding | Location |
|---|------|---------|----------|
| H1 | arch | Binding-model leak (see above): dead `(set,binding)` API, push-constant offsets + buffer indices in the consumer, unenforced per-shader contract | `renderer.cpp:1272-1368`, `rhi.hpp:305,517-527` |
| H2 | vulkan | `recreateSwapchain()` does **not** resize `renderFinishedSemaphores`; if a resize changes the swapchain image count (legal, likelier on MoltenVK), `endFrame` indexes the per-image semaphore vector out of bounds → heap corruption | `rhi_vulkan.cpp:2402` vs `:2574`, used `:1746` |
| H3 | metal | Multi-geometry BLAS build overwrites itself — the per-geometry loop assigns `resource.accelStruct`/`scratchBuffer` each iteration, so a BLAS with N geometries keeps only the last; earlier geometry silently never intersects | `rhi_metal.cpp:1519-1553` |
| H4 | metal | GPU timestamp race — one shared `gpuTimerSampleBuffer` reused across in-flight frames; a late completion handler reads slots a newer frame overwrote → spurious ~200ms pass readings (already diagnosed) | `rhi_metal.cpp:236-259`, `hpp:296` |

### Medium

| # | Area | Finding | Location |
|---|------|---------|----------|
| M1 | both | **`Depth24Stencil8` portability landmine** — unsupported/emulated on Apple Silicon (Metal throws on create), and the Vulkan depth barriers use the DEPTH aspect only, which is wrong for a combined depth+stencil format. Both reviews flagged it independently. `Depth32Float` sidesteps both. | `rhi_metal.cpp:1352`, `rhi_vulkan.cpp:1878,2001,2629` |
| M2 | vulkan | `generateMipmaps` recomputes a full mip chain from dimensions, but the image was created with `desc.mipLevels`; fewer-level textures barrier/blit nonexistent subresources → validation error / UB on MoltenVK | `rhi_vulkan.cpp:1432-1500` |
| M3 | vulkan | `flushDescriptors` hardcodes `SHADER_READ_ONLY_OPTIMAL` for every sampled texture; a compute-written image left in `GENERAL` gets a mismatched descriptor (no path transitions a sampled compute output back to shader-read) | `rhi_vulkan.cpp:577`, `2864,2972` |
| M4 | both | `createTextureView` (this session) — Vulkan double-free is correctly avoided (image/memory null) but a null-image view must never be used as a target (no assert); Metal leaves `format`/`mipLevels` invalid on the view resource. Harmless for the preview-only use, latent otherwise | `rhi_vulkan.cpp:~962`, `rhi_metal.cpp:~414` |
| M5 | vulkan | `updateTexture` always uses `VK_IMAGE_ASPECT_COLOR_BIT`; calling it on a depth texture would issue a color-aspect copy/barrier on a depth image | `rhi_vulkan.cpp:1379-1412` |
| M6 | arch | Capabilities struct is thin (only RT/compute/timestamps) while `MSAA_SAMPLE_COUNT=4` and the Vulkan "8 descriptor slots/set" budget are hardcoded/assumed with no query | `rhi.hpp:395-399`, `renderer.hpp:947`, `renderer.cpp:1323` |
| M7 | arch | `kFrameSlots = 3` hardcoded while `getMaxFramesInFlight()` is dynamic; a backend reporting >3 reintroduces the CPU/GPU race the slotting prevents | `renderer.hpp:515,819` |
| M8 | arch | Parallel Metal-only vs Vulkan-twin pipeline members selected by backend (`ssaoPipeline`/`vkSsaoPipeline`, …); feature availability is expressed two ways (clean `PassFlags` gating *and* `backend == Metal && capabilities.raytracing`) | `renderer.hpp:713-739`, `renderer.cpp:3668` |
| M9 | arch | `*Bytes` inline-constant API leaks Vulkan push-constant packing to callers; `getBackendTexture` returns backend-dependent types (`MTL::Texture*` vs `VkImageView`), forcing consumer branches | `rhi.hpp:530-531,590-599` |

### Low

| # | Area | Finding | Location |
|---|------|---------|----------|
| L1 | vulkan | Staging-ring reclaim has a theoretical hole for uploads recorded *between* frames (reclaim guarded only by `inFlightFences`, which doesn't prove later-submitted upload copies finished). Mid-frame uploads are safe. | `rhi_vulkan.cpp:1628-1651` |
| L2 | vulkan | Redundant-bind filter (this session) trusts non-recycled `VkBuffer`/`VkImageView` values; safe within `MAX_FRAMES_IN_FLIGHT` deferral, low-probability stale-value skip across the retirement boundary | `rhi_vulkan.cpp:2061,2113,2839` |
| L3 | vulkan | `pickPhysicalDevice` takes `physicalDevices[0]` blindly — can select lavapipe/software over a real GPU; `surfaceFormats[0]`/`updateBuffer` double-map/`copySwapchainToBuffer` 32-bit size are the same no-guard class | `rhi_vulkan.cpp:2274,2451,1336,1511` |
| L4 | metal | `currentDrawable` is the one autoreleased object held as a raw pointer (safe only inside the frame pool); TLAS `nextSlot` advances per-build not per-frame; `drawIndexed` hardcodes `UInt32` indices | `rhi_metal.cpp:848,1596,1277` |
| L5 | arch | No-op default virtuals (`setScissor`, `computeBarrier`, `createTextureView`) hide a missing capability as silent wrong behavior rather than a queryable cap; stale handles are silent no-ops (aids safety, hides bugs); `RenderPassDesc::loadColor` is `std::vector<bool>` (per-pass heap alloc) | `rhi.hpp:340,551-561` |

## What is solid (do not "fix")

- **Resource lifetime.** Create/destroy pairing is disciplined; `shutdown()`
  tears down buffers/textures/RTs; the screenshot buffer is destroyed after
  readback; the TLAS is created once then rebuilt in place (no create-in-hot-path
  leaks found). Deferred-retirement timing (`+MAX_FRAMES_IN_FLIGHT`) is sound.
- **Vulkan frame sync.** Frames-in-flight fencing, the per-image present-semaphore
  split, per-frame descriptor-pool reset after the slot's fence wait, and the
  sampled-texture init transition to `SHADER_READ_ONLY` (prevents UNDEFINED at
  submit) are all correct. Shutdown ordering is double-shutdown-safe.
- **Metal ownership.** metal-cpp `TransferPtr` (+1 owned) vs `RetainPtr`
  (autoreleased) is applied correctly at every site; the per-frame autorelease
  pool brackets `beginFrame..endFrame` with all long-lived objects independently
  retained; the upload ring never reuses memory until pending copies complete.
- **Capability-gated RenderGraph.** `RenderGraph` + `PassFlags` +
  `RHICapabilities` is a clean, backend-agnostic way to skip unsupported passes —
  the part of the abstraction that works best.

## Prioritized recommendations

1. **Verify + fix H2 (swapchain semaphore vector)** — small, prevents a
   MoltenVK-triggered crash. Resize `renderFinishedSemaphores` in
   `recreateSwapchain` alongside `swapchainImages`.
2. **Fix H3 (multi-geometry BLAS)** — accumulate all geometry descriptors into
   one array and build once; belongs in the RT/perf session.
3. **Fix H4 (timestamp race)** — per-frame-in-flight ring of sample buffers
   (same partition pattern as the staging ring). Debug-tool-only, but it makes
   the perf panel trustworthy.
4. **Standardize on `Depth32Float` (M1)** — or make depth-stencil aspect handling
   format-aware and gate `Depth24Stencil8` behind a capability. Removes the most
   likely macOS-vs-Linux divergence.
5. **Guard the latent aspect/mip assumptions (M2, M5)** and add the null-image
   assert for views (M4).
6. **RHI v2 — reflection-driven binding (H1)** — the big one. Retires the dead
   API, the ABI-in-consumer leak, the ~40 branches, and the fragile contract.
   Standalone follow-up.

Items H3, H4, and the binding refactor are explicitly out of scope for the
current PR and belong in the RT/perf and "RHI v2" follow-ups.
