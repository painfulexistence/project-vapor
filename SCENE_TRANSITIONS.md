# Scene Transitions

How the UI page system connects to actual scene load/unload.

---

## Overview

A scene transition involves two independent concerns that must be coordinated:

- **UI layer**: showing and hiding `LoadingScreenPage` with fade transitions
- **Engine layer**: unloading the current scene, async-loading the next, rebuilding ECS

The bridge between them is `SceneTransitionComponent`, owned by a persistent entity that outlives any specific scene.

---

## State Machine

```
Idle
  │  (SceneTransitionComponent.targetScene set)
  ▼
FadingInLoadingScreen   — requests LoadingScreenPage::show()
  │  (LoadingScreenPage reaches Visible state)
  ▼
UnloadingScene          — destroys ECS entities, physics bodies, clears scene caches
  │  (one frame, synchronous)
  ▼
LoadingAssets           — ResourceManager::loadScene() async call in flight
  │  (Resource<Scene>::isReady())
  ▼
BuildingScene           — populates ECS from loaded Scene graph (main thread)
  │  (one frame, synchronous)
  ▼
FadingOutLoadingScreen  — requests LoadingScreenPage::hide()
  │  (LoadingScreenPage reaches Hidden state)
  ▼
Idle
```

---

## Components

```cpp
enum class SceneTransitionState {
    Idle,
    FadingInLoadingScreen,
    UnloadingScene,
    LoadingAssets,
    BuildingScene,
    FadingOutLoadingScreen,
};

struct SceneTransitionComponent {
    std::string targetScene;                          // asset path of next scene
    SceneTransitionState state = SceneTransitionState::Idle;
    std::shared_ptr<Resource<Scene>> pendingResource; // in-flight async load
    float progress = 0.0f;                            // 0..1, forwarded to LoadingScreenPage
};
```

`SceneTransitionComponent` lives on a dedicated persistent entity created at startup. It is never destroyed between scenes.

---

## System

```cpp
struct SceneTransitionSystem {
    static void update(entt::registry& reg, EngineCore* engine, PageSystem* pages, float dt);

    // External API — write a request, system handles the rest
    static void requestTransition(entt::registry& reg, const std::string& scenePath);
};
```

`SceneTransitionSystem::update` drives the state machine each frame:

| State | Action |
|-------|--------|
| `FadingInLoadingScreen` | Poll `LoadingScreenPage` state; advance when `Visible` |
| `UnloadingScene` | Destroy all non-persistent entities; clear `ResourceManager` scene cache; advance immediately |
| `LoadingAssets` | Poll `Resource<Scene>::isReady()` / `isFailed()`; update `progress`; advance when ready |
| `BuildingScene` | Call scene builder (equivalent of `buildScene()`); advance immediately |
| `FadingOutLoadingScreen` | Poll `LoadingScreenPage` state; advance when `Hidden`; reset component to `Idle` |

---

## LoadingScreenPage

`LoadingScreenPage` is a `Page` subclass with no separate trigger component — it is driven entirely by `SceneTransitionSystem`.

```cpp
class LoadingScreenPage : public Page {
public:
    void onAttach(Rml::ElementDocument* doc, entt::registry&) override;
    void onDetach() override;
    void onUpdate(float dt) override;

    // Called each frame by SceneTransitionSystem while loading
    void setProgress(float value); // 0..1 — updates progress bar element
};
```

The `.rml` contains a full-screen overlay with a progress bar element. CSS handles the fade transition via `.visible` / `.hidden` classes, consistent with all other pages.

---

## Persistent Entities

Some entities must survive scene transitions (e.g. the player, audio listeners, the `SceneTransitionComponent` owner). Mark them with a tag:

```cpp
struct PersistentTag {};
```

`SceneTransitionSystem` during `UnloadingScene` destroys only entities **without** `PersistentTag`:

```cpp
auto destroyable = reg.view<entt::entity>(entt::exclude<PersistentTag>);
reg.destroy(destroyable.begin(), destroyable.end());
```

---

## Async Loading and Main-Thread Constraint

`ResourceManager::loadScene()` runs on a background thread. ECS entity construction (`buildScene()`) must run on the main thread.

The state machine enforces this naturally:

- `LoadingAssets`: background thread loads the `Scene` graph; main thread only polls `isReady()`
- `BuildingScene`: runs exactly one frame on the main thread after the resource is ready

Do not call `buildScene()` inside the `Resource<Scene>` callback — callbacks fire from the loader thread.

---

## Example Usage

```cpp
// From anywhere (input handler, game logic system, etc.)
SceneTransitionSystem::requestTransition(registry, "assets/scenes/level_02.glb");
```

That's the entire external API. The system handles the rest.

---

## Interaction with Other Pages

During a transition, non-persistent pages (menus, popups) should be closed before `FadingInLoadingScreen` begins, or closed as part of the unload step. `SceneTransitionSystem` can call `PageSystem::closeAll()` at the start of `UnloadingScene` to guarantee a clean state.

`LoadingScreenPage` is the only page active during `LoadingAssets` and `BuildingScene`.
