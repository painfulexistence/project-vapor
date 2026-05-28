# Expected Findings — cpp-review Fixtures

Ground truth for evaluating `/cpp-review` recall and precision.

**How to use:**
```bash
/cpp-review all --dir .claude/commands/cpp-review-fixtures --output actual_findings.md
```
Then compare `actual_findings.md` against this file.

**Metrics:**
- Recall    = (issues found that are in this list) / (total in this list)
- Precision = (issues found that are in this list) / (total issues reported)
- Fix safety = build succeeds after `--fix` on the fixtures directory

A good result: Recall ≥ 0.85, Precision ≥ 0.80. A false positive on any "// OK" line is a precision failure worth investigating.

---

## Style: naming  (`naming_issues.hpp`)

| ID | File | Line(s) | Description |
|----|------|---------|-------------|
| NAM-001 | naming_issues.hpp | 15 | Enum `RenderMode` mixes PascalCase (`Forward`) and UPPER_SNAKE_CASE (`DEFERRED`) values |
| NAM-002 | naming_issues.hpp | 18-19 | Two enums in same file use different value conventions (`VERTEX_BUFFER` vs `Perspective`) |
| NAM-003 | naming_issues.hpp | 23 | Class `render_pipeline` uses snake_case instead of PascalCase |
| NAM-004 | naming_issues.hpp | 25-27 | Mixed member prefixes in `render_pipeline`: `m_width`, `_height`, `depth` |
| NAM-005a | naming_issues.hpp | 30-31 | `visible` is a boolean without `is` prefix, next to `isActive` |
| NAM-005b | naming_issues.hpp | 32 | `enabled` is a boolean without `is` prefix |
| NAM-006a | naming_issues.hpp | 35 | `rid` — opaque abbreviation (should be `resourceId`) |
| NAM-006b | naming_issues.hpp | 36 | `hdr` — opaque abbreviation (should be `isHDR` or `hdrEnabled`) |
| NAM-006c | naming_issues.hpp | 37 | `gfxIdx` — opaque abbreviation (should be `graphicsIndex`) |
| NAM-007 | naming_issues.hpp | 40-42 | `set0s`, `set1s`, `set2s` — numeric suffixes with no semantic meaning |
| NAM-008 | naming_issues.hpp | 52-53 | `graphicsFamilyIdx` and `presentFamilyIndex` use different suffixes (`Idx` vs `Index`) |

**Must NOT be flagged (false positive guard):**
- `enum class GraphicsBackend` (line 22) — internally consistent
- `enum class LoadMode` (line 23) — internally consistent
- `class RenderPipeline` (lines 46-53) — correct naming throughout
- Loop counter parameters `i`, `j` (line 44) — universally understood

---

## Style: cpp  (`cpp_issues.hpp`)

| ID | File | Line(s) | Description | Auto-fixable? |
|----|------|---------|-------------|---------------|
| CPP-001a | cpp_issues.hpp | 14 | `getId()` missing `[[nodiscard]]` | ✅ yes |
| CPP-001b | cpp_issues.hpp | 15 | `isLoaded()` missing `[[nodiscard]]` | ✅ yes |
| CPP-001c | cpp_issues.hpp | 16 | `hasError()` missing `[[nodiscard]]` | ✅ yes |
| CPP-001d | cpp_issues.hpp | 17 | `getPath()` missing `[[nodiscard]]` | ✅ yes |
| CPP-002 | cpp_issues.hpp | 20 | `getCount()` missing `noexcept` | ✅ yes |
| CPP-003 | cpp_issues.hpp | 23 | `getVersion()` missing `const` | ✅ yes |
| CPP-004a | cpp_issues.hpp | 28 | `m_id` (int) uninitialized | ✅ yes — add `= 0` |
| CPP-004b | cpp_issues.hpp | 29 | `m_scale` (float) uninitialized | ✅ yes — add `= 0.0f` |
| CPP-004c | cpp_issues.hpp | 30 | `m_error` (bool) uninitialized | ✅ yes — add `= false` |
| CPP-005 | cpp_issues.hpp | 37 | `NULL` used instead of `nullptr` | ✅ yes |
| CPP-006 | cpp_issues.hpp | 41 | `std::function` member — type-erasure overhead in per-instance data | ⚠️ manual |
| CPP-007 | cpp_issues.hpp | 48-51 | `TextureManager` owns `m_data` via raw pointer, manual new/delete | ⚠️ manual |
| CPP-008 | cpp_issues.hpp | 58 | `M_PI` macro — use `std::numbers::pi_v<float>` or `glm::pi<float>()` | ✅ yes |
| CPP-009 | cpp_issues.hpp | 64 | Raw C array `float positions[3]` — use `std::array<float, 3>` | ✅ yes |
| CPP-010 | cpp_issues.hpp | 72 | `strncpy` — deprecated C API | ⚠️ manual |
| CPP-011 | cpp_issues.hpp | 78 | C-style cast `(float)a` — use `static_cast<float>(a)` | ✅ yes |
| CPP-012 | cpp_issues.hpp | 84 | `index < v.size()` — signed/unsigned comparison | ✅ yes (if index provably ≥ 0) |
| CPP-013 | cpp_issues.hpp | 97-102 | `getViewMatrix()` cannot be `const` because it modifies `_viewMatrix`; should use `mutable` | ⚠️ manual |

**Must NOT be flagged:**
- `tryAcquire()` (line 26) — already has `[[nodiscard]]`, `const`, `noexcept`
- `MeshRenderer` observer pointer (lines 55-59) — non-owning, documented
- `processVertsOk()` (lines 68-71) — uses `std::array` correctly
- `checkBoundsOk()` (lines 88-91) — correctly casts before comparison
- `CameraOk` (lines 107-117) — `mutable` + `const` used correctly

---

## Style: design  (`design_issues.hpp`)

| ID | File | Line(s) | Description | Auto-fixable? |
|----|------|---------|-------------|---------------|
| DES-001 | design_issues.hpp | 14-26 | Two singleton implementations: raw-pointer static (`AudioSystem`) vs Meyers singleton (`PhysicsSystem`) | ⚠️ manual |
| DES-002 | design_issues.hpp | 30-56 | Three managers use three different error strategies: throw, bool return, optional | ⚠️ manual |
| DES-003a | design_issues.hpp | 61-66 | `RenderSystem::init()` has no matching `deinit()` | ⚠️ manual |
| DES-003b | design_issues.hpp | 65 | `RenderSystem::pause()` has no matching `resume()` | ⚠️ manual |
| DES-004 | design_issues.hpp | 77-82 | `MaterialSystem` uses three different ownership models for the same kind of resource | ⚠️ manual |
| DES-005 | design_issues.hpp | 95-105 | `RendererVulkan` exposes concrete public methods not in `RendererBase`, bypassing the abstraction | ⚠️ manual |

**Must NOT be flagged:**
- `NetworkManager` (lines 59-64) — consistent error handling (all return bool)
- `InputManager` (lines 69-72) — symmetric init/deinit pair

---

## Style: duplicates  (`duplicates_issues.hpp` + `legacy_issues.cpp`)

| ID | File(s) | Line(s) | Description | Auto-fixable? |
|----|---------|---------|-------------|---------------|
| DUP-001 | duplicates_issues.hpp:10, legacy_issues.cpp:67 | 10, 67 | `FlyCameraComponent` defined identically in two files | ⚠️ manual |
| DUP-002 | duplicates_issues.hpp | 17 | `GrabberComponent::maxPickupRange` default differs from engine version (5.0 vs 20.0) | ⚠️ manual |
| DUP-003 | duplicates_issues.hpp | 27-55 | `generateTopCapIndices` and `generateBottomCapIndices` have identical bodies | ⚠️ manual |
| DUP-004a | duplicates_issues.hpp | 66-69 | `buildCone()` is a stub — creates empty mesh, immediately returns | ⚠️ manual |
| DUP-004b | duplicates_issues.hpp | 72-75 | `buildTerrain()` is a stub with TODO comment | ⚠️ manual |
| DUP-005 | duplicates_issues.hpp | 88-94 | `TransformSystem::update` duplicated at engine and game layers with incompatible signatures | ⚠️ manual |

**Must NOT be flagged:**
- `generateFanIndices()` (lines 57-64) — similar structure but genuinely different logic
- `buildCube()` (lines 77-83) — complete implementation

---

## Style: legacy  (`legacy_issues.cpp`)

| ID | File | Line(s) | Description | Auto-fixable? |
|----|------|---------|-------------|---------------|
| LEG-001a | legacy_issues.cpp | 27 | `OldAssetManager::loadImage()` (old static API) called alongside new `ResourceManager` | ⚠️ manual |
| LEG-001b | legacy_issues.cpp | 28 | `OldAssetManager::loadMesh()` same issue | ⚠️ manual |
| LEG-002a | legacy_issues.cpp | 38 | TODO comment — ECS migration incomplete | ⚠️ manual |
| LEG-002b | legacy_issues.cpp | 39 | FIXME comment — stagedMeshes should be removed | ⚠️ manual |
| LEG-002c | legacy_issues.cpp | 40 | HACK comment — Node system not yet removed | ⚠️ manual |
| LEG-003 | legacy_issues.cpp | 46-51, 57 | `struct Particle` is marked "legacy" in comment but used in new `spawnParticle()` | ⚠️ manual |
| LEG-004 | legacy_issues.cpp | 67 | `FlyCameraComponent` redefined here (also in `duplicates_issues.hpp`) | ⚠️ manual |
| LEG-005a | legacy_issues.cpp | 87 | Old `Camera` object still constructed in `renderFrame()` alongside `VirtualCameraComponent` | ⚠️ manual |

**Must NOT be flagged:**
- `GPUParticle` (lines 53-57) — the new canonical type, not legacy
- `VirtualCameraComponent` usage (lines 90-91) — the new pattern

---

## Totals

| Style | Total issues | Auto-fixable | Manual |
|-------|-------------|--------------|--------|
| naming | 11 | 0 | 11 |
| cpp | 18 | 11 | 7 |
| design | 6 | 0 | 6 |
| duplicates | 6 | 0 | 6 |
| legacy | 8 | 0 | 8 |
| **total** | **49** | **11** | **38** |

---

## Scoring rubric

| Score | Recall | Precision | Notes |
|-------|--------|-----------|-------|
| Excellent | ≥ 0.90 | ≥ 0.85 | Ship it |
| Good | ≥ 0.80 | ≥ 0.75 | Minor prompt tuning needed |
| Needs work | ≥ 0.65 | ≥ 0.65 | Find the missed categories and add examples |
| Poor | < 0.65 | any | Rewrite the relevant checklist section |

**After `--fix`:** run the project build. Any compile error means a fix rule is too aggressive and must be moved to `[manual]`.
