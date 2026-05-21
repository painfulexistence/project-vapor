# ADR-005: Phase 4 Refactoring Decisions

**Date:** 2026-05-01  
**Status:** Accepted

## Context

Phase 4 targets surgical cleanup of the highest-value technical debt in the demo layer (Vaporware/) without touching engine internals that lack test coverage. Four changes were made: strong-typed physics handles (T1), systems naming normalization (T3), RmlUI helper extraction (T4), and SceneBuilder extraction (T2).

---

## T1 — `PhysicsHandle<Tag>` template

**Decision:** Replace the two identical `BodyHandle`/`TriggerHandle` bare structs with a tag-template:

```cpp
template<typename Tag>
struct PhysicsHandle {
    Uint32 rid = UINT32_MAX;
    bool valid() const { return rid != UINT32_MAX; }
    bool operator==(const PhysicsHandle&) const = default;
};
struct BodyTag {};
struct TriggerTag {};
using BodyHandle    = PhysicsHandle<BodyTag>;
using TriggerHandle = PhysicsHandle<TriggerTag>;
```

**Why:** The original bare structs were structurally identical, so `physics.destroyBody(triggerHandle)` compiled without error. The template makes accidental cross-type assignment a compile error at zero runtime cost. This mirrors the existing `GPUHandle<Tag>` pattern in `graphics_handles.hpp` (ADR-004).

**Impact:** Pure type-alias change. All call sites keep the same names. No logic changes. Forward declarations of `BodyHandle` as a struct (e.g., in `character_controller.hpp`) had to be replaced with a full include because type aliases cannot be forward-declared.

---

## T3 — Static-class pattern for ECS systems

**Decision:** Convert all 8 free functions in `systems.hpp` from `updateXSystem(...)` to `XSystem::update(...)` static-class form, matching the existing 3 classes already present.

**Why:** The mixed convention required two grep patterns to find all systems. A single `*System::update` pattern now finds everything. The rename also makes it easier to add per-system state later without breaking call sites.

**Impact:** Pure rename. No logic changes. All call sites in `main.cpp` updated.

---

## T4 — RmlUI helper functions over base class

**Decision:** Extract two helpers into `namespace RmlUIHelpers` at the top of `systems.hpp`:
- `ensureDocument(docPtr, rml, path)` — loads a document once, returns the pointer
- `tickTimer(timer, dt, duration)` — advances a timer and returns true when it fires

**Why not a base class?** The 5 RmlUI systems (HUD, ScrollText, Letterbox, Subtitle, ChapterTitle) have different state enums, different element lookups, and different transition logic. A base class would need virtual dispatch and shared state that doesn't cleanly unify. Two free helpers eliminate ~60 lines of copy-paste without introducing coupling. `tickTimer` was applied where the pattern was `timer += dt; if (timer >= duration)` with no interrupt check between them; interrupt-checked states were left as-is to avoid semantic drift.

---

## T2 — `buildScene` free function, not class

**Decision:** Extract the 195-LOC entity-creation block from `main()` into `Vaporware/src/scene_builder.hpp` as a single `buildScene(...)` free function returning a `SceneResources` struct.

**Why free function, not class:** The scene builder has no persistent state — it runs once, populates the registry, and returns handles. A class would add boilerplate (constructor, member variables) with no benefit. The `SceneResources` return struct carries the two cross-cutting handles `main()` needs: `cube1` (referenced by `FollowCameraComponent::target`) and `global` (the singleton entity for switch requests).

**Impact:** `main()` reduced from 627 to 435 lines. No new dependencies introduced; `scene_builder.hpp` includes only headers already used in `main.cpp`.

---

## Known remaining debt

- **ECS/SceneGraph dual model** (§6.5 ARCHITECTURE.md): Vaporware uses both EnTT components and `Scene` nodes for the same objects. Unification would require touching engine internals without test coverage. Deferred.
- **Vulkan backend parity**: 28 Metal render passes have no Vulkan counterpart. Tracked in `parity-matrix.md`. Deferred to a dedicated graphics sprint.
