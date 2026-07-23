# Animation & UI Convergence — Vapor ⇄ Atmospheric

The two engines built the same two stacks from opposite ends, and the halves
line up almost perfectly:

- **Vapor** has the *top* layers: a declarative UI page/navigation layer
  (`PageSystem` + blueprint-declared pages) and an FSM-driven cinematic
  orchestration layer (subtitle queues, letterbox, chapter titles). Its *time
  primitives* underneath are hand-rolled: per-page fade state machines,
  `std::function` easings, an imperative `shared_ptr<Action>` graph.
- **Atmospheric** has the *bottom* layers: a data-driven animation core
  (`AnimationLibrary` clips referenced by handles, `AnimatorComponent`
  playback, central `AnimationSubsystem` tick, 31 CSB-compatible easings, the
  `Tween` builder) and a self-contained `UIPageComponent` document lifecycle.
  It has *no* navigation stack, no page registry, and no orchestration layer
  above the components.

This document plans what each side borrows from the other, and settles the
`Rml::ElementDocument*` question: on the Vapor side the pointer should become
a handle so UI data components stay plain data; on the Atmospheric side the
pointer is fine where it is, because `UIPageComponent` is the document's
lifetime owner by design.

This is a plan, not a changelog — nothing below is implemented yet.

---

## 1. Current state

### 1.1 Vapor (ECS, entt) — strong on top, ad-hoc below

| Layer | What exists | Where |
|---|---|---|
| UI navigation | `PageID` registry, menu stack (`push/pop/popAll`), `UIVisibleTag` intent, lazy load | `Vapor/include/Vapor/ui_page_system.hpp` |
| UI data/behavior split | `UIDocumentComponent` (path + raw `doc` pointer) vs `UIPageBehaviorComponent` (`shared_ptr<Page>`) | same |
| Declarative wiring | `uiPage` blueprint applier: page factory by name, navigator wiring, atlas-by-name precedent | `Vaporware/src/main.cpp` (`registerAppBlueprintComponents`) |
| Cinematic orchestration | FSM components + split sensor/timer/action systems (subtitles), trigger components (chapter title), scroll text queue | `Vapor/include/Vapor/fsm.hpp`, `Vaporware/src/systems.hpp` |
| Time primitives | `ActionManager` (`Timeline/Parallel/Repeat/Delay/Callback/Update` over `shared_ptr<Action>`), owned by `EngineCore` | `Vapor/include/Vapor/action_manager.hpp` |
| Easing | 8 inline functions + `EasingFunc = std::function<float(float)>` | same |
| Sprite animation | `FlipbookComponent` carries its *own* frame list per entity; game-side `FlipbookSystem` ticks it | `components.hpp:452`, `Vaporware/src/systems.hpp` |
| Page transitions | every page hand-rolls a `State` enum + timer (`LetterboxPage`, `ChapterTitlePage`, …) and hand-rolls `Rml::EventListener` glue (`MainMenuPage::bind`, `PauseMenuPage`, `SettingsPage` — three copies) | `Vaporware/src/pages/*.hpp` |

### 1.2 Atmospheric (OOP, GameObject/Component) — strong below, missing the top

| Layer | What exists | Where |
|---|---|---|
| Clip assets | `FlipbookClip` / `ActionTimeline` / `SkeletonClip` / VAT — data-only, shared, referenced by typed handles (`TimelineHandle{u32 id}`, id 0 invalid) | `Engine/include/Atmospheric/animation_clip.hpp` |
| Clip store | `AnimationLibrary` (stable addresses, by-name lookup, scene-scoped `Clear`) | `animation_subsystem.hpp` |
| Playback | `AnimatorComponent` base: playhead + speed + `WrapMode{Once,Loop,PingPong,ClampHold}` + finished/looped events; `Evaluate(time)` is a pure function of time (seek/reverse/scrub for free) | `animator_component.hpp` |
| Central tick | `AnimationSubsystem::Process` advances all players; global + per-group time scales | `animation_subsystem.hpp` |
| Tweens | `ActionTimelineComponent` (named timelines + fire-and-forget overlays) + `Tween` fluent builder; timeline `eventId`s and custom-track sinks | `action_timeline_component.hpp` |
| Easing | `EasingType` enum, 31 values, CSB-compatible numbering, `ApplyEasing` | `easing.hpp` |
| UI page | `UIPageComponent`: loads document on `OnAttach`, `OnDocumentLoaded` hook, `AddListener` helpers that *own* the `Rml::EventListener` adapters and tear them down on detach | `ui_page_component.hpp/.cpp` |
| Missing | page registry, navigation stack, overlay semantics, visibility-as-intent, lazy load, FSM orchestration | — |

### 1.3 The complementarity, stated once

Vapor knows **how UI flows are structured** (registry, stack, intent tags,
FSM orchestration, declarative wiring). Atmospheric knows **how time is
sampled** (clips as assets, handles, one tick point, pure-function-of-time
evaluation) and **how RmlUi glue should be owned** (`AddListener`). Each side's
weakness is the other side's core. The plan below transplants structure
downward (Vapor → Atmos) and primitives upward (Atmos → Vapor), keeping each
engine's object model (entt ECS vs GameObject/Component) intact.

---

## 2. Vapor borrows from Atmospheric

### 2.1 Data-driven animation core (replaces hand-rolled timers, not FSMs)

Port the *shape* of Atmos's stack into entt idioms:

- `AnimationClipLibrary` owned by `EngineCore` (next to `ActionManager`):
  stores `ActionTimeline`-style clips (keyframe tracks + events) and flipbook
  clips; by-name lookup returning typed handles, mirroring the existing
  atlas-by-name → `AtlasHandle` pattern in the blueprint appliers.
- A PoD playback component — this is the Vapor translation of
  `AnimatorComponent`'s `PlaybackState`:

  ```cpp
  struct TimelinePlaybackComponent {
      TimelineHandle clip;            // library handle, u32
      float time  = 0.0f;
      float speed = 1.0f;
      WrapMode wrap = WrapMode::Once;
      bool playing = false;
      Uint32 groupId = 0;             // time-scale group (int, not string — PoD)
  };
  ```

- `TimelineSystem::update(reg, library, dt)`: one tick point. Applies
  Position/Rotation/Scale tracks to `TransformComponent`, Color/Alpha to
  `Sprite2D/Sprite3DComponent` tint, fires crossed `eventId`s (as FSM events
  into `FSMEventQueue` — this is where the two stacks meet), routes Custom
  tracks to a per-entity sink. Global + per-group time scale like
  `AnimationSubsystem` (pause menu open → world group paused, UI group
  running).

Explicit non-goal: the FSM orchestration (subtitle sensor/timer/action
systems) **stays**. It is the correct layer and it is Vapor's strength. What
changes is what's underneath it: pages stop counting `timer_ += dt` by hand
and instead either watch a clip's playback state or drive RCSS classes from
timeline events.

### 2.2 One easing vocabulary

Replace `Vapor::Easing`'s 8 inline functions + `std::function` alias with
Atmos's `EasingType` enum + `ApplyEasing(t, type)`, keeping Atmos's numeric
values (they are CSB-compatible and become the shared vocabulary across both
engines). An enum is PoD, serializable in blueprints, and PFR-inspectable;
`std::function` is none of those. Keep the old namespace as thin wrappers
during migration.

### 2.3 `Page::bind` — adopt the `AddListener` ownership pattern

Three Vaporware pages hand-roll the same `ClickListener` +
`vector<pair<Element*, unique_ptr<Listener>>>` glue. Move Atmos's
`UIPageComponent::AddListener` design into the `Vapor::Page` base:
`bind(elementId, event, std::function<void()>)`, adapters owned by the base,
torn down automatically in `onDetach`. This also makes re-attach (needed for
hot reload, §4) safe by construction.

### 2.4 Flipbooks become shared clips (later)

`FlipbookComponent` currently carries its frame list per entity. Move frame
data into `AnimationClipLibrary` as `FlipbookClip` (Atmos's
`FromTileset`/`FromGrid` constructors are worth taking verbatim); the entity
keeps only `{FlipbookClipHandle, playhead}`. Low urgency, do last.

### 2.5 What `ActionManager` becomes

`ActionManager` stays as the *imperative escape hatch* — code-driven
sequencing of callbacks (level scripting, one-shot engine glue). It stops
being the answer for anything an artist/designer would author or anything
that needs scrubbing/reverse: those are clips. Document this boundary in its
header; optionally add a `PlayTimelineAction(handle)` adapter so imperative
sequences can embed data-driven clips.

---

## 3. Atmospheric borrows from Vapor

### 3.1 `UINavigator` — registry + stack + overlays

A small service (Application-owned, `Get()` locator like the other
subsystems) translating `PageSystem`:

```cpp
class UINavigator {
public:
    using PageId = uint32_t;               // opaque; games define constants
    void Register(PageId, UIPageComponent*);
    void Show(PageId);  void Hide(PageId); // intent, not immediate
    void Push(PageId);  void Pop();  void PopAll();   // menu stack
    bool IsTopOfStack(PageId) const;  bool IsStackEmpty() const;
};
```

Deliberate fix while borrowing: Vapor's `PageID` is an engine enum hardcoding
game pages (`HUD`, `PauseMenu`, …) — a layering wart. Atmos should take
opaque `uint32_t` ids from day one, and Vapor should migrate its enum to
game-side registration in a later phase (§5).

### 3.2 Visibility-as-intent + lazy load

`UIPageComponent` today loads and `Show()`s in `OnAttach`, and `Hide()` is
immediate. Borrow Vapor's split between *intent* (`targetVisible`,
`UIVisibleTag`) and *presentation*: add `lazyLoad` (don't load until first
shown) and route `Show/Hide` through intent + overridable `OnShow/OnHide`
hooks. That is what makes animated hide possible — and Atmos can implement
the animation better than Vapor does today, by driving document opacity or an
RCSS class from its own `Tween`/`ActionTimeline` instead of a hand timer.

### 3.3 FSM orchestration kit

Vapor's `FSMDefinition`/`FSMDefinitionBuilder` (shared definition, string
events, guarded + timed transitions) ports cleanly as plain data; wrap it in
an `FSMComponent : Component` that owns `currentState` + an event queue and
ticks itself. Combined with `ActionTimelineComponent`'s `eventId` callbacks
and the navigator, this reproduces Vapor's subtitle/letterbox cinematic
pattern (sensor → event → transition → action) in OOP form. Port the
subtitle-queue example as the reference sample.

### 3.4 Declarative page wiring

Vapor's `uiPage` blueprint applier (page factory by name, navigator wired at
instantiation) is worth mirroring in `scene_loader`/prefabs once the
navigator exists: pages declared in data, behavior classes resolved by name.

---

## 4. RmlUi document lifetime — handle it (in Vapor)

### 4.1 Why the raw pointer is wrong in Vapor's component

`UIDocumentComponent.doc` is a raw `Rml::ElementDocument*` inside a
blueprint-authored ECS data component. Concretely:

1. **Ownership confusion.** The Rml `Context` owns documents; `Close()`
   defers actual destruction to the next `Context::Update`. Any cached
   pointer — `UIDocumentComponent.doc`, `Page::doc_`, and every
   `Rml::Element*` a page caches (`ChapterTitlePage::container_`,
   `SubtitlePage`, `ScrollTextPage`, `LoadingScreenPage`, the menu pages'
   listener pairs) — dangles the moment anything closes or reloads the
   document. `RmlUiManager::ReloadDocument` exists (and string-matches by
   path to find the old document) but is unusable for pages today because
   nothing fixes up those cached pointers: hot reload is one `Close()` away
   from UB.
2. **Not data.** The blueprint applier authors `{path, lazyLoad}`; `doc` is
   runtime state smuggled into the authored component. PFR-based inspection
   silently skips pointer fields; serialization can't round-trip it; copying
   the component silently duplicates a non-owning pointer.
3. **Tests already say so.** `tests/ui_system_test.cpp` injects
   `reinterpret_cast<Rml::ElementDocument*>(0xdeadbeef)` to bypass loading —
   the classic smell of a resource reference that should be a handle.

Vapor already solved this three times: `PhysicsHandle<Tag>{Uint32 rid}`,
`GPUHandle<Tag>`, `AtlasHandle`. Documents are the one asset type still
referenced by raw pointer from a data component.

### 4.2 Design: `UIDocumentHandle` + registry in `RmlUiManager`

```cpp
struct UIDocumentHandle {
    Uint32 slot = UINT32_MAX;   // house sentinel, matches PhysicsHandle/GPUHandle
    Uint32 gen  = 0;            // bumped on close → stale handles resolve null
    bool valid() const { return slot != UINT32_MAX; }
    bool operator==(const UIDocumentHandle&) const = default;
};
```

Unlike bodies/atlases, documents are closed and *reloaded* as a designed
workflow (hot reload), so this handle carries a generation, plus a separate
per-slot **version** counter that survives reload:

```cpp
// Pure bookkeeping, no Rml calls — unit-testable without Rml::Initialise().
class UIDocumentRegistry {
public:
    UIDocumentHandle insert(Rml::ElementDocument* doc, std::string path);
    Rml::ElementDocument* resolve(UIDocumentHandle) const; // null if stale
    const std::string*    path(UIDocumentHandle) const;
    Uint32                version(UIDocumentHandle) const; // ++ on replace()
    void replace(UIDocumentHandle, Rml::ElementDocument* newDoc); // hot reload
    void erase(UIDocumentHandle);                          // gen++, freelist
};
```

`RmlUiManager` owns one registry and wraps it:

- `LoadDocumentHandle(path) → UIDocumentHandle`
- `Resolve(h) → Rml::ElementDocument*` / `DocumentVersion(h)`
- `CloseDocument(h)` — `Close()` the Rml doc *and* `erase` the slot in the
  same call, so the handle goes stale immediately (never resolvable during
  Rml's deferred teardown window)
- `ReloadDocument(h)` — close old, load same path, `replace()` into the
  *same slot*: the handle every component holds stays valid, `version`
  bumps. No more find-by-path string matching.

The existing raw-pointer `LoadDocument/UnloadDocument` stay as shims during
migration (the CAPI `UIRenderer` is a separate standalone consumer with its
own contexts — out of scope, untouched).

### 4.3 Component and `PageSystem` changes

```cpp
struct UIDocumentComponent {
    std::string path;
    UIDocumentHandle doc;      // value-semantic, inspector/serializer-friendly
    Uint32 seenVersion = 0;    // last version this page attached against
    bool lazyLoad = false;
    bool lastSentVisible = false;
};
```

`PageSystem::update` per entity:

1. `!doc.valid()` and should load → `doc = rml->LoadDocumentHandle(path)`,
   attach page.
2. `auto* d = rml->Resolve(doc)`; null (closed externally) → clear handle,
   allowing reload next frame under the same lazy rules.
3. `rml->DocumentVersion(doc) != seenVersion` → `page->onDetach()` then
   `page->onAttach(d, reg)`, update `seenVersion`. **This is the line that
   makes RML hot reload actually work end-to-end**: pages re-resolve their
   `Element*` caches in `onAttach`, and the `bind()` helper (§2.3) guarantees
   listeners were detached first.

`Page` keeps a `doc_` convenience pointer but the contract changes to:
*valid only between `onAttach` and `onDetach`; element pointers may be cached
only in `onAttach`; re-attach may happen at any time*. Add a `drawField`
case for `UIDocumentHandle` (like `BodyHandle`) so the inspector shows
validity instead of skipping the field.

Sequencing note: keep the component an aggregate (PFR/blueprint fill), and
update the two designated-init sites (`main.cpp` applier, `ui_system_test`)
in the same commit — field order `path, doc, seenVersion, lazyLoad, …`.

Test story: `UIDocumentRegistry` gets its own unit test (fake pointers are
legitimate there — it never dereferences). `ui_system_test` seeds a real
registry through a test accessor instead of casting `0xdeadbeef`, and gains
the two new cases: stale-handle resolve after close, reattach-on-version-bump.

### 4.4 Atmospheric: keep the pointer, fix the reload hook

`UIPageComponent` is not, and should not be, PoD — it is a behavior object
with virtuals whose *job* is owning the document's lifetime (`OnAttach`
loads, `OnDetach` closes; listeners torn down first). An internal owning
pointer in the lifetime owner is correct. Handle-izing it would add
indirection with no consumer: nothing serializes or reflects over that field.

What Atmos *should* take from this section is the reload contract, because
subclasses cache `Rml::Element*` (e.g. MultiplayerSandbox's HUD): add
`UIPageComponent::Reload()` that clears listeners, closes, reloads the same
path, and re-fires `OnDocumentLoaded()`. Same invariant as Vapor's
version-bump reattach, one object instead of a registry. If Atmos ever grows
a data-driven scene format that authors pages, adopt the same
`{slot, gen}` handle shape then — not before.

### 4.5 The PoD boundary, stated once

- Vapor **data components**: asset references are handles (atlas ✓, body ✓,
  document after §4.2); no owning raw pointers, no `std::function`, no
  virtuals. `std::string` paths and small vectors are fine (aggregate,
  PFR-friendly).
- Vapor **behavior**: lives behind `UIPageBehaviorComponent`'s
  `shared_ptr<Page>` — deliberately not PoD; that split already exists and is
  correct. Handles don't cross into it; it resolves through `PageSystem`.
- Atmos: PoD pressure lives in the clip/asset layer (already handles);
  components are lifetime owners by design.

---

## 5. Phasing

Each phase is independently landable and testable.

**Vapor**

| Phase | Contents | Tests |
|---|---|---|
| V1 | `UIDocumentHandle` + `UIDocumentRegistry`; manager APIs; component + `PageSystem` reattach-on-version; `Page::bind` helper (prerequisite for safe reattach) | registry unit test; `ui_system_test` de-fake + stale/reload cases |
| V2 | `EasingType`/`ApplyEasing` unification; `AnimationClipLibrary` + `TimelinePlaybackComponent` + `TimelineSystem` (transform/sprite tracks, events → `FSMEventQueue`, group time scales); port `LetterboxPage`/`ChapterTitlePage` timing onto it | clip sampling test (mirror Atmos's `animation_smoke_test`); timeline→FSM event test |
| V3 | Flipbook clips into the library; `PageID` → game-side registration; `ActionManager` boundary docs + optional clip-playing action | flipbook migration test |

**Atmospheric**

| Phase | Contents | Tests |
|---|---|---|
| A1 | `UINavigator` (opaque ids, stack, overlays); `UIPageComponent` lazy load + intent + `OnShow/OnHide`; `Reload()` re-firing `OnDocumentLoaded` | navigator stack unit test (mirrors Vapor's `ui_system_test` cases) |
| A2 | FSM kit port (`FSMDefinition` + builder + `FSMComponent`); subtitle-queue cinematic sample on navigator + timeline events | FSM transition test (port `fsm_system_test` cases) |

**Shared vocabulary (both repos, enforced by convention):** `WrapMode`
member set, `EasingType` numeric values (CSB numbering), the
"library owns clips / components own playheads" principle, and handle shape
`{u32 (+gen where hot-swap is a workflow)}`. House sentinels stay per-repo
(Atmos `id == 0`, Vapor `UINT32_MAX`) — semantic parity matters, spelling
doesn't.

## 6. Risks / notes

- Rml's deferred document destruction: `CloseDocument` must erase the slot
  *before* returning; `resolve` must never hand out a closing document.
- `onAttach` idempotency: pages must tolerate re-attach at any frame; the
  `bind()` helper and "cache elements only in `onAttach`" contract are what
  make that safe. Audit the five element-caching pages when landing V1.
- `ReloadDocument`'s current path-string matching (documents found via
  `GetDocument(path)`) disappears with the registry — one less stringly-typed
  invariant.
- Group time scales interact with `FSMTimedTransition` (it advances on real
  dt): decide in V2 whether FSM state time respects animation groups
  (recommended: yes, via the same scaled dt the timeline system uses).
