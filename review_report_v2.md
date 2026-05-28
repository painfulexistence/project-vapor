# C++ Code Review Report — v2

**Date:** 2026-05-28
**Scope:** Full codebase (post-merge: `feat: scene serializer and scene.json`)
**Active styles:** naming, design, legacy, duplicates, cpp, security

---

## Summary

The main merge introduced `SceneSerializer`, refactored `Vapor::systems.hpp` to use `entt::registry&` directly (eliminating the engine/game signature mismatch for `TransformSystem`), and extracted `setupCustomDrawers()` from `main.cpp`. These are genuine improvements. However, the merge also introduced **three new uninitialized bool/enum members** in `CharacterIntent` and `LightMovementLogicComponent` that cause undefined behaviour on first read. Four component types remain duplicated verbatim between the engine and game layers. The `CameraSystem` duplication persists in a new form: it now exists under two different namespaces with different signatures. Legacy `struct Particle` and all `void*` mapped buffers remain untouched.

> Items marked **[NEW]** were introduced by the merge. Items marked **[FIXED]** were resolved. Items marked **[PERSISTS]** were in the previous report and remain.

---

## Style: Naming

### A. Enum value conventions — three incompatible styles coexist

**[PERSISTS]** Same as v1. Within the same codebase:
- `GraphicsBackend` / `RenderPath` / `MotionType` use `PascalCase` values
- `BufferUsage` / `RenderTargetUsage` / `PrimitiveMode` / `AlphaMode` use `UPPER_SNAKE_CASE`
- `MovementPattern` (game layer) uses `PascalCase`

No single convention has been adopted.

### B. Class named with underscore

**[PERSISTS]** `renderer_vulkan.hpp:20`
```cpp
class Renderer_Vulkan final : public Renderer {
```
Only class in the codebase using snake_case. Should be `RendererVulkan`.

### C. Member variable prefix — four styles still in use

**[PERSISTS + PARTIAL IMPROVEMENT]**
- `scene_inspector.hpp` — now consistently uses `m_` for all private members ✅
- `scene_serializer.hpp` — uses `m_` consistently ✅
- `engine_core.hpp` — mixes `_` prefix (`_taskScheduler`) with `s_` prefix (`s_instance`)
- `physics_3d.hpp` — uses `_` prefix (`_instance`)
- `Vaporware/src/components.hpp` — no prefix on any member

Four conventions across the project: `m_`, `_`, `s_`, none.

### D. Abbreviations

**[NEW]** `scene_serializer.hpp:119`
```cpp
struct WriterEntry {
    std::string     key;
    ComponentWriter fn;   // ← `fn` is an opaque abbreviation; should be `writer` or `callback`
};
```

**[PERSISTS]** `graphics_handles.hpp:11` — `rid` (should be `resourceId`), `renderer_vulkan.hpp:243-244` — `graphicsFamilyIdx` vs `presentFamilyIndex` (mixed suffix).

### E. Boolean naming inconsistency

**[PERSISTS]** `visible` (no prefix) next to `isActive`, `isActive` next to `enabled`, `jumpRequested` without prefix — same set of inconsistencies as v1.

### F. Chinese comments in engine headers

**[PERSISTS]** `Vapor/include/Vapor/systems.hpp:14,42,104,153`
```cpp
// 變換系統 - 計算世界變換矩陣 (EnTT 版)
// 渲染系統 - 收集渲染實例 (EnTT 版)
// 物理同步系統 - 同步 Transform 和 Physics (EnTT 版)
// 相機系統 (EnTT 版)
```
Engine public headers use Chinese section comments; all other comments in the codebase are English.

---

## Style: Design

### A. Singleton — two implementations

**[PERSISTS]** `engine_core.hpp:78` uses `static EngineCore* s_instance` (raw pointer, `s_` prefix); `physics_3d.hpp:92` uses `static Physics3D* _instance` (raw pointer, `_` prefix). Same pattern, different naming convention.

### B. Error handling — three strategies

**[PERSISTS + NEW VARIANT]**

| Subsystem | Strategy | Location |
|-----------|----------|----------|
| `DefinitionDatabase` | throws `std::runtime_error` | `definition.hpp:185` |
| `AudioEngine::init()` | returns `bool` | `audio_engine.hpp` |
| `SceneSerializer::save()` | returns `SaveResult{ok, error, count}` | `scene_serializer.hpp:76` **[NEW]** |
| Most other managers | no error indication | various |

`SaveResult` is the most ergonomic of the three, but adding a fourth distinct pattern increases the inconsistency.

### C. CameraSystem duplicated across namespaces with incompatible signatures

**[NEW FORM — PERSISTS IN SPIRIT]**

| Location | Class | Signature |
|----------|-------|-----------|
| `Vapor/include/Vapor/systems.hpp:157` | `Vapor::CameraSystem` | `update(entt::registry&, InputManager&, float)` |
| `Vaporware/src/systems.hpp:268` | `::CameraSystem` (global ns) | `update(entt::registry&, float)` |

Both named `CameraSystem`. The game's version handles input via `CharacterIntent` component instead of `InputManager`. A reader searching for "CameraSystem" will find two incompatible implementations.

Note: `TransformSystem` duplication from v1 is now **[FIXED]** — engine uses `entt::registry&` directly, game layer removed its own `TransformSystem`.

### D. `SceneInspector` holds raw observer pointer to `SceneSerializer`

**[NEW]** `scene_inspector.hpp:67`
```cpp
SceneSerializer* m_serializer = nullptr;  // non-owning observer
```
The ownership contract is stated in a comment (`// The serializer must outlive this inspector`), which is acceptable for an observer pointer. However, there is no way to verify this at compile time. A `std::reference_wrapper<SceneSerializer>` or requiring serializer via constructor would make the contract explicit.

### E. `Renderer` abstract interface has two incompatible `draw()` overloads

**[PERSISTS]** `renderer.hpp:48-49` — both `draw(Scene, Camera)` and `draw(registry, Scene, Camera)` are pure virtual, signalling an unresolved architecture decision between scene-graph and ECS rendering.

---

## Style: Legacy

### A. `struct Particle` — legacy CPU type still present

**[PERSISTS]** `graphics_effects.hpp:249-254`
```cpp
// Legacy CPU particle (kept for compatibility with older systems)
struct Particle {
    glm::vec3 position = glm::vec3(1.0f);  // ← initial value 1.0 is wrong (should be 0)
    glm::vec3 velocity = glm::vec3(1.0f);
    glm::vec3 density  = glm::vec3(1.0f);  // ← density as vec3 has no physical meaning
};
```
`GPUParticle` is the current system. This struct should be deleted.

### B. `TODO`/`FIXME` in active code

**[PERSISTS]** `Vaporware/src/systems.hpp:119`
```cpp
// TODO: batch create
```
`BodyCreateSystem::update()` processes one body per frame iteration; batch creation is the intended path but not implemented.

**[PERSISTS]** `definition.hpp:154-170` — three stub methods (`loadFromFile`, `loadFromJson`, `loadFromBinary`) with `[[maybe_unused]]` parameters and empty bodies.

### C. `stagedMeshes` workflow

**[FIXED]** — removed in main merge. ✅

### D. Old `Camera` class alongside `VirtualCameraComponent`

**[PERSISTS]** The old `Camera` class (direct position/target/fov constructor) remains in the engine. `VirtualCameraComponent` is the ECS approach. `main.cpp` still includes `Vapor/camera.hpp` and the legacy `Camera` is still used in `renderer_vulkan.cpp` and `renderer_metal.cpp` for the `draw(Scene, Camera)` overload.

---

## Style: Duplicates

### A. Four components defined in both engine and game layer

**[PERSISTS — CRITICAL]**

| Component | Engine (`Vapor::`) | Game (global ns) | Difference |
|-----------|-------------------|-------------------|------------|
| `FlyCameraComponent` | `components.hpp:72-77` | `Vaporware/components.hpp:71-77` | None — exact copy |
| `FollowCameraComponent` | `components.hpp:79-84` | `Vaporware/components.hpp:79-85` | None — exact copy |
| `HeldByComponent` | `components.hpp` | `Vaporware/components.hpp:43-47` | None — exact copy |
| `GrabberComponent` | `components.hpp` (`maxPickupRange=20.0f`) | `Vaporware/components.hpp:49-52` (`maxPickupRange=5.0f`) | Default value differs |

The game `#include`s `"Vapor/components.hpp"` then redefines the same structs in the global namespace. The engine versions and the game versions are distinct types — they cannot be used interchangeably with EnTT.

### B. `GrabbableComponent` vs `GrabberComponent` — two grab representations

**[NEW]** `Vaporware/src/components.hpp:36-41` adds `GrabbableComponent` (on the grabbed object: `pickupRange`, `holdOffset`, `throwForce`, `isHeld`). The engine already has `GrabberComponent` (on the grabber entity) and `HeldByComponent` (on the held entity). The game now has three grab-related components, two of which overlap in semantics (`GrabbableComponent::isHeld` mirrors `HeldByComponent`).

### C. `CharacterMovementSystem` — empty jump handler

**[PERSISTS]** `Vaporware/src/systems.hpp:260-262`
```cpp
if (intent.jump) {
    // ← body is empty
}
```

### D. `CameraSystem::update` matrix computation duplicated

**[NEW]** The last 4 lines of `Vapor::CameraSystem::updateMatrices()` (`systems.hpp:239-247`) and the last 4 lines of game `CameraSystem::update()` (`Vaporware/src/systems.hpp:304-307`) are byte-for-byte identical:
```cpp
glm::mat4 rotation    = glm::mat4_cast(cam.rotation);
glm::mat4 translation = glm::translate(glm::mat4(1.0f), cam.position);
cam.viewMatrix        = glm::inverse(translation * rotation);
cam.projectionMatrix  = glm::perspective(cam.fov, cam.aspect, cam.near, cam.far);
```
This is already in a helper in the engine — the game should call the engine's `CameraSystem::update()` or `Vapor::CameraSystem::updateMatrices()` instead of duplicating it.

### E. `setupCustomDrawers` — 15 structurally identical lambda blocks

**[PARTIAL IMPROVEMENT]** — previously 16 inline lambdas in `main()`, now extracted into `setupCustomDrawers()`. The repeated structural pattern remains but is at least grouped. The pattern `try_get → CollapsingHeader → ImGui fields` repeats 15 times with no abstraction.

---

## Style: C++ Patterns

### A. Uninitialized bool members — undefined behaviour on read

**[NEW — CRITICAL]** `Vaporware/src/components.hpp:24-26`
```cpp
struct CharacterIntent {
    glm::vec2 lookVector      = glm::vec2(0.0f);
    glm::vec2 moveVector      = glm::vec2(0.0f);
    float     moveVerticalAxis = 0.0f;
    bool jump;      // ← uninitialized
    bool sprint;    // ← uninitialized
    bool interact;  // ← uninitialized
};
```
Reading any of these three booleans before assignment is UB. `CharacterMovementSystem::update()` reads `intent.jump` (line 260) unconditionally.

### B. Uninitialized enum member

**[NEW — HIGH]** `Vaporware/src/components.hpp:58`
```cpp
struct LightMovementLogicComponent {
    MovementPattern pattern;   // ← no initializer; reading before write is UB
    float speed  = 1.0f;
    float timer  = 0.0f;
    ...
};
```
`LightMovementSystem::update()` reads `logic.pattern` in a `switch` without a prior write guarantee.

### C. Missing `[[nodiscard]]` on key methods

**[NEW]** `scene_serializer.hpp:76` — `save()` returns `SaveResult` with `ok` and `error`; callers who ignore it silently discard errors.

**[NEW]** `Vapor/include/Vapor/systems.hpp:181` — `getActiveCamera()` returns `entt::entity`; silently ignoring it is a logic bug.

**[PERSISTS]** `resource_manager.hpp` — `isReady()`, `isFailed()`, `isLoading()`, `get()`. `input_manager.hpp` — `isHeld()`, `isPressed()`, `isReleased()`. `camera.hpp` — `getViewMatrix()`, `isVisible()`.

### D. `strncpy` — deprecated C API

**[PERSISTS]** `scene_inspector.hpp:53, 228`
```cpp
strncpy(m_gltfPathBuf, path.c_str(), sizeof(m_gltfPathBuf) - 1);
strncpy(buf, c.name.c_str(), sizeof(buf) - 1);
```
These are not null-terminator-safe in all implementations. Use `std::string` operations or `std::copy_n` + explicit null-terminator.

### E. Raw C char arrays for path buffers

**[PERSISTS]** `scene_inspector.hpp:62, 69-71`
```cpp
char m_searchBuf[128]  = {};
char m_savePath[256]   = "scene.json";
char m_gltfPathBuf[256] = "";
```
These are passed directly to ImGui `InputText`, which requires mutable char buffers — an ImGui API constraint. This is a known ImGui limitation; document it explicitly.

### F. `void*` mapped GPU buffers — six raw void* vectors

**[PERSISTS]** `renderer_vulkan.hpp:306-312`
```cpp
std::vector<void*> cameraDataBuffersMapped;
std::vector<void*> instanceDataBuffersMapped;
std::vector<void*> directionalLightBuffersMapped;
std::vector<void*> pointLightBuffersMapped;
std::vector<void*> particleSimParamsBuffersMapped;
std::vector<void*> particleAttractorBuffersMapped;
```

### G. `M_PI` macro — not standard C++

**[PERSISTS]** `mesh_builder.hpp` — used 6+ times. Replace with `glm::pi<float>()` (glm is already a dependency).

### H. Singleton raw pointers

**[PERSISTS]** `engine_core.hpp:78` and `physics_3d.hpp:92` — raw pointer singletons, no RAII.

### I. Lazy-init getters missing `mutable`

**[PERSISTS]** `camera.hpp` — `getViewMatrix()`, `getProjMatrix()`, `getFrustumPlanes()` modify members but cannot be `const`. Should mark internal matrices as `mutable`.

### J. `std::function` as ECS component callback

**[PERSISTS]** `components.hpp:127-128` — `TriggerVolumeComponent::onEnter` and `onExit` are `std::function`, adding heap allocation per component instance.

### K. `Material` owns `PipelineHandle`

**[PERSISTS]** `graphics_mesh.hpp:56` — material data struct holds a renderer-managed resource.

---

## Style: Security

### A. Uninitialized reads — undefined behaviour

**[NEW — CRITICAL]** Covered above in §C++/A: `CharacterIntent::jump`, `sprint`, `interact` are read before guaranteed initialization.

### B. Signed/unsigned comparison — multiple new instances

**[NEW]** `Vaporware/src/systems.hpp:162`
```cpp
if (ref.lightIndex < 0 || ref.lightIndex >= scene->pointLights.size())
//                         ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^ int vs size_t
```

**[NEW]** `Vaporware/src/systems.hpp:47, 330, 334, 357` — `flipbook.currentIndex >= flipbook.frameIndices.size()` and `q.currentIndex < (int)q.queue.size()` (C-style cast).

**[PERSISTS]** `Vapor/include/Vapor/systems.hpp` had signed/unsigned comparisons; now removed in the refactor — ✅ improved.

### C. C-style casts on size comparison

**[NEW]** `Vaporware/src/systems.hpp:330, 334, 357`
```cpp
q.currentIndex < (int)q.queue.size() - 1   // C-style cast
(int)q.queue.size()                          // repeated pattern
```
Should use `static_cast<int>`.

### D. Unchecked vector access

**[NEW]** `Vaporware/src/systems.hpp:164`
```cpp
auto& light = scene->pointLights[ref.lightIndex];  // operator[] after bounds check — OK
```
Bounds check at line 162 uses signed/unsigned comparison (see §B), so if `lightIndex` wraps, the check passes incorrectly before the access.

### E. Lambda `[&]` capture with non-trivial lifetime

**[PERSISTS]** `scene_inspector.hpp` and `main.cpp` — long-lived callbacks registered via `registerCustomDrawer([&](...){...})` capture locals by reference. Callers must ensure the captured environment outlives the inspector.

---

## Severity Table

| # | Issue | Severity | Location | Status |
|---|-------|----------|----------|--------|
| 1 | `CharacterIntent::jump/sprint/interact` uninitialized bools — UB on read | **CRITICAL** | `Vaporware/components.hpp:24-26` | 🆕 NEW |
| 2 | Four engine components duplicated verbatim in game layer | **CRITICAL** | `Vaporware/components.hpp` vs `Vapor/components.hpp` | PERSISTS |
| 3 | `CameraSystem` exists in two namespaces with incompatible signatures | **HIGH** | `systems.hpp:157`, `Vaporware/systems.hpp:268` | 🆕 NEW FORM |
| 4 | `LightMovementLogicComponent::pattern` uninitialized enum | **HIGH** | `Vaporware/components.hpp:58` | 🆕 NEW |
| 5 | Signed/unsigned comparison in `LightMovementSystem` bounds check | **HIGH** | `Vaporware/systems.hpp:162` | 🆕 NEW |
| 6 | `CharacterMovementSystem::update` — empty jump handler | **HIGH** | `Vaporware/systems.hpp:260-262` | PERSISTS |
| 7 | `save()` missing `[[nodiscard]]` — serialization errors silently dropped | **HIGH** | `scene_serializer.hpp:76` | 🆕 NEW |
| 8 | Six `void*` mapped buffer vectors — no type safety | **HIGH** | `renderer_vulkan.hpp:306-312` | PERSISTS |
| 9 | Singleton raw pointers — no RAII | **HIGH** | `engine_core.hpp:78`, `physics_3d.hpp:92` | PERSISTS |
| 10 | `GrabbableComponent` overlaps semantically with `HeldByComponent` | **HIGH** | `Vaporware/components.hpp:36-41` | 🆕 NEW |
| 11 | Matrix computation copy-pasted between engine and game `CameraSystem` | **HIGH** | `systems.hpp:239-247`, `Vaporware/systems.hpp:304-307` | 🆕 NEW |
| 12 | Enum value conventions — three styles (`PascalCase`, `UPPER_SNAKE_CASE`, none-sep) | **HIGH** | `renderer.hpp`, `physics_3d.hpp`, `graphics_mesh.hpp` | PERSISTS |
| 13 | Error handling — three strategies (`throw`, `bool`, `SaveResult`) | **HIGH** | `definition.hpp`, `audio_engine.hpp`, `scene_serializer.hpp` | PERSISTS + GREW |
| 14 | Legacy `struct Particle` — comment says legacy, still compiled | **MEDIUM** | `graphics_effects.hpp:249` | PERSISTS |
| 15 | `Renderer::draw()` has two overloads — unresolved architecture | **MEDIUM** | `renderer.hpp:48-49` | PERSISTS |
| 16 | `Material` owns `PipelineHandle` — violated SRP | **MEDIUM** | `graphics_mesh.hpp:56` | PERSISTS |
| 17 | C-style casts `(int)q.queue.size()` — use `static_cast` | **MEDIUM** | `Vaporware/systems.hpp:330,334,357` | 🆕 NEW |
| 18 | `strncpy` in `scene_inspector.hpp` | **MEDIUM** | `scene_inspector.hpp:53,228` | PERSISTS |
| 19 | `getActiveCamera()` missing `[[nodiscard]]` | **MEDIUM** | `systems.hpp:181` | 🆕 NEW |
| 20 | 20+ getters missing `[[nodiscard]]` | **MEDIUM** | `resource_manager.hpp`, `input_manager.hpp`, `camera.hpp` | PERSISTS |
| 21 | `WriterEntry::fn` — opaque abbreviation | **MEDIUM** | `scene_serializer.hpp:119` | 🆕 NEW |
| 22 | Member prefix styles — four conventions across codebase | **MEDIUM** | various | PERSISTS |
| 23 | Chinese comments in engine public headers | **MEDIUM** | `systems.hpp:14,42,104,153` | PERSISTS |
| 24 | `Renderer_Vulkan` — only snake_case class name | **MEDIUM** | `renderer_vulkan.hpp:20` | PERSISTS |
| 25 | Lazy-init getters not `const` — should use `mutable` | **MEDIUM** | `camera.hpp` | PERSISTS |
| 26 | `std::function` in ECS component callbacks | **LOW** | `components.hpp:127-128` | PERSISTS |
| 27 | `M_PI` macro — use `glm::pi<float>()` | **LOW** | `mesh_builder.hpp` (6+ uses) | PERSISTS |
| 28 | Boolean naming `is` prefix inconsistency | **LOW** | various | PERSISTS |
| 29 | Definition stubs `loadFromFile/Json/Binary` with `[[maybe_unused]]` | **LOW** | `definition.hpp:154-170` | PERSISTS |

**Totals:** CRITICAL: 2 | HIGH: 12 | MEDIUM: 11 | LOW: 4

---

## Changes since v1

| Category | Fixed ✅ | New 🆕 | Unchanged |
|----------|---------|--------|-----------|
| CRITICAL | 0 | 1 (uninitialized bools) | 1 (component duplicates) |
| HIGH | 1 (TransformSystem sig mismatch) | 5 | 6 |
| MEDIUM | 0 | 4 | 7 |
| LOW | 0 | 0 | 4 |

Net issue count: **38 → 29** (consolidated + 9 fixed from refactor, but 6 new issues added).

---

## Highest-priority fix

**Initialize `CharacterIntent::jump`, `sprint`, `interact` to `false`** (`Vaporware/src/components.hpp:24-26`). These three bools are read by `CharacterMovementSystem` and displayed in the inspector without any guaranteed prior assignment, which is undefined behaviour. One-line fix each; should be applied immediately.
