# Scene Management & Instantiation Convergence — Vapor ⇄ Atmospheric

Companion to `animation-ui-convergence.md`, same shape of finding: the two
engines built the same system from opposite ends, and this pair has already
been cross-pollinated once — both repos carry a `SceneBlueprint` with a
parse → load-resources → instantiate split, ported in an earlier pass. The
implementations have since drifted into perfect complements:

- **Vapor** finished the *data model*: every entity of the shipped demo lives
  in `scenes/main.json` (typed flat blueprint, PFR auto-appliers, binary cook,
  async loading). But the scene loads exactly once — the engine has **no
  scene switching**. The vocabulary for it exists and is dead: a six-state
  transition FSM is authored, `PersistentTag` is registered, a
  `LoadingScreenPage` ships — and no system executes any of it, because the
  renderer's geometry pool is append-only and nothing owns the swap protocol.
- **Atmospheric** finished the *runtime protocol*: `GoScene` (two-stage async
  prefetch → unload → load → instantiate under a loading overlay),
  `ReloadScene`, additive `AddScene`/`UnloadScene(name)` with per-scene asset
  scoping. But its blueprint holds **raw JSON blobs per entity**, is not
  cookable, and component coverage is 15 hand-written factory lambdas out of
  ~50 component types — the "全部 entity 進 JSON" property Vapor has is
  exactly what Atmos lacks.

Each side's missing half is the other side's finished half. This document
plans the exchange. Nothing below is implemented yet.

---

## 1. Current state

### 1.1 Vapor — declarative all the way down, static forever

| Piece | State | Where |
|---|---|---|
| Blueprint | Typed & flat: parent-indexed `EntityBlueprint`s + typed payload vectors (meshes/materials/images/lights, now skeletons/clips). Pure parse; `source`/`prefab` refs expanded by the loader; splice via `appendBlueprint` | `scene_blueprint.hpp/.cpp` |
| Component coverage | `BlueprintComponents` PFR auto-applier — any aggregate component registers in one line; hand appliers only for runtime-setup components (bodies, UI pages, atlas-by-name). The demo's whole world is authored in `scenes/main.json`; `scene_builder.hpp` was retired | `scene_blueprint.hpp`, `Vaporware/src/main.cpp` |
| Cook | `.vscene` binary (cereal) guarded by an input-bytes freshness hash; `BLUEPRINT_FORMAT_VERSION` versioned | `asset_serializer.cpp` |
| Async load | `ResourceManager::loadScene` (sync/async via TaskScheduler) | `resource_manager.hpp` |
| Scene save | `SceneSerializer` writes registry → scene JSON (editor save path) | `scene_serializer.hpp` |
| Switching | **Absent.** One `loadScene` + `instantiate` + `renderer->stage(scene)` at startup. `SceneTransitionComponent` + 6-state FSM (`Idle → FadingInLoadingScreen → UnloadingScene → LoadingAssets → BuildingScene → FadingOutLoadingScreen`) are *defined* and inspector-visible, but no system drives them; `PersistentTag` has no consumer; `Renderer::stage`/`registerMesh` have **no unstage/unregister**, `RenderScene` has no reset | `Vaporware/src/components.hpp`, `renderer.cpp` |

### 1.2 Atmospheric — full lifecycle, thin data

| Piece | State | Where |
|---|---|---|
| Switching | `GoScene(name, onReady)`: prefetch scene JSON → parse → prefetch declared assets → `UnloadCurrentScene` → `LoadSceneResources` → `InstantiateScene` → `onReady`, all under the loading overlay (`SceneTransition` presentation class). WASM-safe by construction (prefetch model). `ReloadScene()` re-runs it | `application.cpp` |
| Additive scenes | `AddScene(json)` instantiates under a named container GameObject; `UnloadScene(name)` destroys that subtree and frees the assets *first loaded by that scene* (`AssetManager::UnloadSceneAssets`); `ClearScenes` sweeps all | same |
| Asset scoping | AssetManager tracks scene-tier assets per scene name; `ClearSceneAssets` preserves defaults. AnimationLibrary is likewise scene-scoped (`Clear()` on unload) | `asset_manager.hpp` |
| Blueprint | `SceneBlueprint` = resource declarations (textures/shaders/materials/meshes) + `EntityBlueprint { nlohmann::json resolvedData }` — per-entity **raw JSON blobs**. The Phase-1 "resolve prefab refs" step is a stated TODO; nothing is cookable or serializable back | `scene_blueprint.hpp` |
| Component coverage | `ComponentFactory` + format-agnostic `Deserializer` — a good registry design, but every component needs a hand-written creator lambda: **15 registered** (Transform, Sprite, Text2D/3D, Camera, CameraController3D, Light, VoxelVolume, StreamingTerrain, VoxelWorld, MeshInstancer, ShapeRenderer, MeshRenderer, Flipbook, ActionTimeline) vs ~50 component types. Rigidbody, particles, water, sun, portal, skeletal, UIPage, … are code-spawn only | `component_factory.hpp`, `application.cpp` `RegisterComponents` |
| Known warts | `GameObject` destruction does not run `OnDetach` — `UnloadCurrentScene` hand-clears every graphics list to un-dangle the renderer (the comment admits it). `Scene`/`SceneNode` (scene.hpp) is a vestigial empty class (`scene.cpp` is 0 lines) | `application.cpp:1769`, `scene.hpp` |

### 1.3 The complementarity, stated once

Vapor knows **what a scene is** (typed, cookable, reflection-covered data
that round-trips authoring → cook → instantiate → editor-save). Atmospheric
knows **how scenes live** (the unload/teardown ordering, per-scene asset
scoping, additive load, async prefetch under an overlay, reload). Vapor even
authored the transition FSM it never wired up — the plan below wires it with
Atmos's protocol; and Atmos already carries Vapor's phase split — the plan
below fills it with Vapor's data model.

---

## 2. Vapor borrows from Atmospheric: the scene lifecycle

### 2.1 V-S1 — teachable teardown: `RenderScene::clear` + `Renderer::unstageAll`

The blocker for any switching is the append-only world-geometry pool
(`registerMesh` has no inverse). For whole-scene swaps, a **full pool reset**
is sufficient and honest — it is exactly what Atmos's `ClearSceneAssets`
does at the asset tier:

- `Renderer::unstageAll()`: wait for in-flight frames, release the pool's
  GPU buffers (vertex/index/meshlet/MDI), clear CPU mirrors, and invalidate
  `renderMeshId` on every previously staged `Mesh` so a later re-stage
  re-registers cleanly.
- `RenderScene::clear()`: staged lists, payload mirrors, light lists.
- Persistent entities that carry world geometry get re-staged after the
  reset (their meshes are alive in `MeshRendererComponent`s; only the pool
  entry died). First iteration may simply forbid `PersistentTag` on
  geometry-carrying entities and assert.

Per-mesh freeing (freelist/compaction for additive unload) is a follow-up,
not a prerequisite.

### 2.2 V-S2 — `SceneManager`: drive the FSM that already exists

An engine-side manager (owned like the other EngineCore services, but
operating on the game's registry/RenderScene/renderer, passed in) that
finally *executes* `createSceneTransitionFSM()`'s states — the Atmos GoScene
protocol expressed in Vapor's own FSM vocabulary:

| FSM state (already authored) | Action (new) |
|---|---|
| `FadingInLoadingScreen` | `PageSystem::show(LoadingScreen)`; sensor waits for the page's fade (same sensor pattern as the subtitle systems) |
| `UnloadingScene` | destroy every entity without `PersistentTag` (**PersistentTag finally gets its consumer**); reactive cleanup systems already handle the rest (`BodyDestroySystem` frees Jolt bodies — the precedent); then `unstageAll` + scene-scoped asset clear: `AnimationClipLibrary::clear()` (its comment already promises "scene-scoped, Clear() on scene unload"), atlas/image caches per policy |
| `LoadingAssets` | `ResourceManager::loadScene(target, Async)` — the async machinery exists; progress lands in `SceneTransitionComponent.progress` (the loading page's bar) |
| `BuildingScene` | `instantiate` + `renderer->stage` + the post-instantiate wiring hook (`onReady` callback — camera aspect, page push, name lookups; today's inline main.cpp code becomes the first callback) |
| `FadingOutLoadingScreen` | hide page, sensor waits, back to `Idle` |

API: `SceneManager::request(reg, "scenes/chapter2.json")` pushes
`StartTransition` — everything else is data + systems. UI pages survive the
swap by being authored `"persistentTag": {}` in the scene JSON (pages are
entities; persistence is data, no special case).

### 2.3 V-S3 — additive scenes (later)

`instantiate()` already takes a parent and returns a root: additive load =
instantiate under a named root entity, unload = destroy that subtree
(mirrors Atmos `AddScene`/`UnloadScene(name)`). Needs per-mesh unstaging or
per-scene pools — schedule after V-S1/V-S2 prove out, alongside per-scene
asset scoping keyed by that root.

---

## 3. Atmospheric borrows from Vapor: the data model

### 3.1 A-S1 — coverage: PFR-fill the Props structs

Atmos components are OOP (can't PFR the component), but nearly every creator
lambda is mechanical `d.Read(field)` into an **aggregate Props struct**
(`SpriteProps`, `MaterialProps`, …) — and aggregates are exactly what
Boost.PFR fills. Port Vapor's `registerComponent`/`readField` at the Props
level:

```cpp
// One line per component, like Vapor's BlueprintComponents:
ComponentFactory::RegisterProps<SpriteComponent, SpriteProps>("SpriteComponent");
// PFR fills SpriteProps by JSON key (same readField skip rules: handles,
// containers, computed state are not authorable), then new SpriteComponent(o, props).
```

Hand creators remain for components needing runtime resolution (texture
paths → handles — the atlas-by-name pattern), taking the PFR-filled props as
a base. Then audit the ~35 unregistered components; the goal is Vapor parity:
*anything spawnable is authorable*. (Keep the `Deserializer` abstraction —
its format-agnosticism is a genuine Atmos advantage; PFR slots in as the
JSON implementation's default field filler.)

### 3.2 A-S2 — finish Phase 1, then cook

Two steps, independently landable:

1. **Resolve, don't defer**: implement the stated TODO — prefab refs merge
   during Phase 1, so `resolvedData` is genuinely resolved and Phase 2 never
   touches the filesystem. This also unblocks nested prefabs (Vapor's
   `appendBlueprint` splice semantics are the reference).
2. **Cook**: port Vapor's `.vscene` idea — a composed, versioned binary
   artifact guarded by an input-bytes freshness hash. On WASM (Atmos's
   primary deploy) this collapses the per-scene JSON parse + N prefetch
   round-trips into one artifact fetch: the biggest practical win of the
   whole exchange. Staged adoption is fine: cook the composed JSON + a
   prefetch manifest first; move to the typed flat layout only if parse time
   still matters.

### 3.3 A-S3 — scene save-back

Atmos's editor loads scenes (`LoadEditorScene`/`AddScene`) but cannot write
them. Port `SceneSerializer`'s shape (per-component writer registry,
game-extensible) so the editor loop closes: load → tweak in inspector →
save. Pairs naturally with A-S1 (writers and fillers are the same field
lists — with PFR they can be literally the same code).

### 3.4 A-S4 — hygiene enabled by the exchange

- Run `OnDetach` on GameObject destruction (component-lifecycle TODO), then
  delete the hand-clearing of graphics lists in `UnloadCurrentScene` —
  Vapor's reactive-cleanup convention is the model; unload should be
  "destroy entities, let lifecycles fire", not "and also remember six lists".
- Retire the vestigial `Scene`/`SceneNode` (scene.hpp, empty scene.cpp) —
  Atmos's real scene state is Application's entity list + containers, and
  the dead class only misleads readers (Vapor removed its equivalent when
  the ECS took over; the header comment there says why).

---

## 4. Shared vocabulary

- **Transition states**: Vapor's FSM names (`FadingInLoadingScreen`,
  `UnloadingScene`, `LoadingAssets`, `BuildingScene`,
  `FadingOutLoadingScreen`) are the shared protocol vocabulary; Atmos's
  GoScene stages map onto them 1:1 and its `SceneTransition` overlay is the
  presentation of the same states. Keep the names aligned in logs/UI.
- **Persistence**: Vapor `PersistentTag` (component) ↔ Atmos survives-scene
  set (today: only `_defaultGameObject`). When Atmos grows authored
  persistence, spell it `"persistent": true` in entity JSON and map to the
  same semantics (skip on unload; re-parent to root container).
- **Scene JSON schemas stay per-repo** (Vapor: `components` object keyed by
  name; Atmos: typed entries in `resolvedData`) — assets exist on both
  sides; unify semantics (persistence, prefab refs, name-based asset
  references), not spelling.
- **Asset tiers**: "default/engine tier survives, scene tier clears" is the
  shared rule (Atmos `ClearSceneAssets` ↔ Vapor V-S2's library/atlas clear).

## 5. Phasing

| Phase | Contents | Tests |
|---|---|---|
| V-S1 | `Renderer::unstageAll` + `RenderScene::clear` + `renderMeshId` invalidation | headless: stage → unstage → restage, ids reassigned, no leak (buffer count) |
| V-S2 | `SceneManager` driving the existing FSM; PersistentTag consumer; scene-scoped clears; loading-page progress; main.cpp's inline load becomes the first `onReady` | FSM-driven swap test with mock renderer (particle-test precedent); persistent-entity survival test |
| V-S3 | additive scenes + per-mesh unstage | subtree unload test |
| A-S1 | `RegisterProps` PFR filler + coverage audit | golden-scene JSON round-trip per component |
| A-S2 | Phase-1 prefab resolution; cook + freshness hash | cook/re-cook freshness test |
| A-S3 | scene save-back | load → save → load equivalence |
| A-S4 | OnDetach-on-destroy + delete hand-clears; retire `Scene`/`SceneNode` | existing examples still boot; UnloadCurrentScene diff shrinks |

## 6. Risks / notes

- **GPU teardown ordering** (V-S1): `unstageAll` must fence in-flight frames
  before freeing pool buffers; on Metal/Vulkan that means waiting the frame
  ring out. Do it inside the loading-screen states where a hitch is invisible
  (the same trick Atmos's comment notes — "runs under the loading overlay").
- **Persistent geometry** (V-S2): entities that survive with meshes need
  re-staging after the pool reset — start by asserting the combination away.
- **Cook vs live-edit** (A-S2): the freshness hash must include prefab files
  once Phase 1 merges them (Vapor's `sources` list is the pattern).
- **PFR on Props** (A-S1): props containing GPU handles/computed members need
  the skip rules (Vapor's `readField` already encodes them; `Hidden<T>` has
  no Atmos equivalent yet — skip-by-type covers most cases).
- **Registry-wide destroy** (V-S2): order matters — entity destruction must
  run before renderer reset (reactive systems like BodyDestroy read
  components during their pass), mirroring Atmos's "GameObjects first, then
  asset frees" comment in `UnloadScene`.
