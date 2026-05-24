# Architecture: Project Vapor

> 分析日期：2026-04-29（最後更新：2026-05-01）
> 分析來源：source code（headers + 實作檔案），不依賴 README

---

## 1. 專案整體結構

```
project-vapor/
├── Vapor/                  # 引擎核心函式庫（static/shared library）
│   ├── include/Vapor/      # 公開 API（34 個 headers，2026-05-01）
│   └── src/                # 實作（24 個 .cpp 檔）
│
├── Vaporware/              # Demo 應用程式（引擎的使用者）
│   └── src/
│       ├── main.cpp        # 應用程式入口 + 遊戲迴圈 (~25k LOC)
│       ├── components.hpp  # Demo 專用 ECS 元件定義
│       └── systems.hpp     # Demo 專用 ECS 系統 (~23k LOC)
│
├── tests/                  # Catch2 v3 測試套件（2026-05-01 新增）
│   ├── CMakeLists.txt
│   ├── action_system_safeguard_test.cpp
│   ├── scene_transform_safeguard_test.cpp
│   ├── camera_safeguard_test.cpp
│   ├── physics_safeguard_test.cpp      # 需要 Vapor lib（Jolt init）
│   └── resource_manager_safeguard_test.cpp  # 需要 Vapor lib
│
└── .github/workflows/ci.yml  # GitHub Actions CI（macos-15, Ninja, ccache）
```

Vapor（library）和 Vaporware（application）是分離的 CMake target。引擎不依賴 demo；demo 依賴引擎。

---

## 2. 核心模組與職責

```
                        ┌─────────────────────────────────────────┐
                        │              EngineCore                  │
                        │  (singleton 協調者，擁有所有子系統)         │
                        └──────────┬──────────────────────────────┘
                                   │ owns
         ┌─────────────────────────┼─────────────────────┐
         ▼                         ▼                       ▼
  TaskScheduler             ResourceManager          ActionManager
  (enkiTS worker             (async/sync cache        (easing + timer
   thread pool)               Image/Scene/Mesh)        + chaining)
         │
         ├──────────────────────────────────────────┐
         ▼                                          ▼
  InputManager                               AudioManager
  (SDL event → action mapping,               (miniaudio backend,
   state + history buffer)                    2D/3D spatial audio)
                                                     │
                                             RmlUiManager (optional)
                                             (HTML/CSS HUD)


  Physics3D                    Scene / Node Graph
  (Jolt Physics + enkiTS        (hierarchical transforms,
   job system, singleton)        GPU vertex/index buffers,
         │                       lights, fluid volumes)
         │ drives
  CharacterController            Renderer (abstract)
  VehicleController               ├── RendererMetal (Metal backend)
                                  └── RendererVulkan (Vulkan backend)
```

---

## 3. 資料流：一幀的生命週期

```
SDL_PollEvents()
      │
      ├─→ InputManager::processEvent()    // 鍵盤/滑鼠事件 → InputState
      └─→ EngineCore::processRmlUIEvent() // UI 優先消費

EngineCore::update(dt)
      ├─→ ActionManager::update(dt)       // 推進 tweens/timers
      ├─→ AudioManager::update(dt)        // 觸發完成 callback
      └─→ ResourceManager (async tasks 在 TaskScheduler 執行)

Physics3D::process(scene, dt)            // 固定 60Hz 步進
      ├─→ JoltEnkiJobSystem (parallel)   // Broadphase + narrowphase
      ├─→ ContactListener callbacks       // onCollisionEnter/Exit
      ├─→ TriggerListener callbacks       // onTriggerEnter/Exit
      └─→ 更新 Node::localTransform（從 physics body 讀回位置）

Scene::update(dt)
      └─→ 遞迴更新 Node world transforms（只有 dirty 的 node）

Renderer::draw(scene, camera)
      ├─→ (若 isGeometryDirty) Stage buffers to GPU
      ├─→ Pre-pass (depth prepass)
      ├─→ Cluster light culling compute
      ├─→ PBR forward pass (instanced)
      ├─→ Sky / atmosphere pass
      ├─→ Water pass
      ├─→ Volumetric fog/cloud/god-ray pass
      ├─→ Post-process: Bloom → DOF → Tone mapping
      ├─→ 2D/3D batch flush
      ├─→ UI pass (RmlUI)
      └─→ Debug draw (optional wireframe)
```

---

## 4. 核心抽象設計

### 4.1 Renderer 抽象

`Renderer`（`renderer.hpp`）是純虛介面。Metal 和 Vulkan 各實作一套，透過 `createRenderer(GraphicsBackend)` factory 在執行時選擇。

引擎程式碼只使用 `Renderer*`，不知道底層 API。這是「最脆弱」的邊界：Metal 實作（562 LOC header）暴露了 16 個 render pass 的具體型別，任何 pass 變動都須在兩個後端同步。

### 4.1a Graphics API 模組化（2026-05-01 新增）

原始 `graphics.hpp`（544 LOC God Header）已被拆分為五個獨立 sub-header：

```
graphics.hpp (umbrella, 16 LOC)
├── graphics_handles.hpp   — GPUHandle<Tag> 模板，type-safe pipeline/buffer/texture/render-target handles
├── graphics_batch2d.hpp   — BlendMode enum, Batch2DVertex, Batch2DUniforms, Batch2DStats
├── graphics_gpu_structs.hpp — GPU layout: MaterialData, lights, CameraData, InstanceData, Cluster, LightCullData
├── graphics_mesh.hpp      — CPU-side: VertexData, WaterVertexData, Mesh, Material, Image, AlphaMode
└── graphics_effects.hpp   — Effects: WaterData, AtmosphereData, VolumetricFogData, VolumetricCloudData,
                              SunFlareData, LightScatteringData, GPUParticle, ParticleSimulationParams
```

**影響**：`graphics.cpp` 現在只 include `graphics_mesh.hpp`（只需要 Mesh 實作）。舊程式碼 include `graphics.hpp` 維持完全相容。這修正了 §6.3 中「任何改動都會觸發大量重新編譯」的問題。

### 4.2 Node 場景圖

`Node` 使用 `shared_ptr` 樹狀結構。`localTransform`（mat4）由使用者設定；`worldTransform` 在 `Scene::update()` 中延遲計算（`isTransformDirty` flag）。

物理 body 的位置每幀從 Jolt 讀回，寫進 `Node::localTransform`，再由場景圖計算 world transform——單向資料流，引擎不允許直接修改物理 body 的 transform（除非用 `Physics3D::setPosition`）。

### 4.3 Resource<T> 模板

`Resource<T>` 是 future-like 物件：建立時為 `Loading` 狀態，完成後切換為 `Ready`。`get()` 阻塞；`tryGet()` 非阻塞。`ResourceCache<T>` 按路徑去重，防止同一資源載入兩次。

### 4.4 Handle 系統

物理 body、trigger、texture、font、audio 全部使用 `uint32_t` 包裝的 handle（`BodyHandle`、`TriggerHandle`、`TextureHandle`…）。`UINT32_MAX` 為 sentinel 值表示無效。

**技術債（部分修復）**：GPU resource handles 已透過 `GPUHandle<Tag>` 模板取得編譯期型別安全（`graphics_handles.hpp`）。但物理 body/trigger handles 仍是裸 `uint32_t` 包裝，型別系統無法防止混用——這個問題僅在 graphics 層修復，physics 層仍待處理。

---

## 5. 關鍵依賴圖

```
Vapor（library）依賴：
  SDL3          → 視窗管理、事件、計時
  glm           → 數學（vec3、mat4、quat）
  Jolt Physics  → 3D 剛體模擬
  enkiTS        → Task scheduler（Jolt 並行 + 資源非同步載入）
  EnTT          → ECS（在 Vaporware demo 層使用）
  miniaudio     → 音訊引擎（single-header）
  RmlUi         → HTML/CSS UI
  tinygltf      → GLTF 場景載入
  stb_image     → 圖片解碼
  MikkTSpace    → 切線空間計算
  Tracy         → 效能剖析
  ImGui         → 開發者 debug UI

Graphics backends：
  Metal         → macOS（metal-cpp wrapper）
  Vulkan        → 跨平台（glslang 編譯 SPIR-V）
```

---

## 6. 最脆弱 / 最混亂的區域

### 6.1 Vaporware/src/main.cpp — 單一 25k LOC 怪獸

**問題**：所有遊戲初始化、場景建置、主迴圈邏輯、相機控制、UI 動畫全部堆在一個檔案。這不是引擎的問題，是 demo 的問題——但它使「引擎的正確使用方式」完全不可見。

**風險**：無法寫整合測試、無法重用任何遊戲邏輯。

### 6.2 Renderer 實作的 pass 數量失控

`renderer_metal.hpp`（562 LOC）forward-declare 了 31 個 render pass struct。每次新增視覺效果，就在兩個後端（Metal + Vulkan）各加一個 pass，且這些 pass 沒有共同介面——只靠呼叫順序隱性耦合。

**信號**：Vulkan 後端的 header 只有 192 LOC，功能明顯落後 Metal，顯示 Vulkan 是「第二公民」。Vulkan 目前只有 3 個 render pass 對 Metal 的 31 個，且 2D/3D batch API、字型渲染、UI、光追、體積效果全部缺失。

詳見 `parity-matrix.md` 的完整功能 × 後端對照表。

### 6.3 ~~graphics.hpp — 543 LOC 的 God Header~~（已修復，2026-05-01）

原本 40+ struct 塞在單一 header 的問題已透過拆分為 5 個聚焦 sub-header 解決（見 §4.1a）。`graphics.hpp` 現在是 16 LOC 的 umbrella include，僅在需要整個 graphics API 時使用。

### 6.4 Handle 型別安全缺失

`BodyHandle`、`TriggerHandle` 底層都是 `uint32_t`。現有 code 靠命名慣例避免混用，但沒有編譯期保護。

### 6.5 ECS 使用不一致

Demo 用 EnTT 的 ECS（`components.hpp`、`systems.hpp`），但引擎的 Scene Graph 用 `shared_ptr` 樹。兩套物件模型並存，場景物件有時是 `Node*`，有時是 `entt::entity`，沒有統一映射機制。

---

## 7. 測試套件（2026-05-01 更新）

**Phase 2 防護網已部分建立。** Catch2 v3 測試框架已整合，CI 在 GitHub Actions (macos-15) 自動執行。

### 已覆蓋（68 個測試案例，3 個 binary）

| Binary | 覆蓋範圍 | 測試數 |
|--------|----------|--------|
| `test_action_system` | Easing×7、Timer×8、Delay/Callback/TimedCallback/Update/Timeline/Parallel/Repeat Actions、ActionManager | 38 |
| `test_scene_transform` | Node transform 分解、dirty flag、世界 transform 傳播（3層）、findNode | 16 |
| `test_camera` | 建構/投影模式、dirty-flag 快取、getForward、frustum planes、isVisible（bsphere + AABB） | 14 |

**覆蓋率狀態**：ActionSystem / SceneGraph / Camera 關鍵路徑已達 Phase 2 門檻（80%）。

### 尚未覆蓋（明確延後）

- **GPU/Renderer**：Metal/Vulkan backend 無法在 headless CI 初始化——所有渲染 pass、批次繪製、字型渲染均未測試。
- **Physics3D**：`physics_safeguard_test.cpp` 存在但需要完整 Jolt init，CI 目前不執行此 binary。
- **AudioManager**：需要音訊裝置，CI 環境不可用。
- **ResourceManager**：`resource_manager_safeguard_test.cpp` 存在但需要實際檔案系統資產，CI 不執行。
- **RmlUI / InputManager**：純邏輯部分尚未測試。

詳見 `notes/safeguard_coverage.md`。

### Physics3D 初始化重構（2026-05-01）

`Physics3D` 的 job system 已從自訂 `JoltEnkiJobSystem`（整合 enkiTS）改為 Jolt 原生 `JPH::JobSystemThreadPool`：

```cpp
// Before
jobSystem = std::make_unique<Vapor::JoltEnkiJobSystem>(taskScheduler, 2048);

// After
jobSystem = std::make_unique<JPH::JobSystemThreadPool>(
    JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers,
    std::thread::hardware_concurrency() - 1);
```

同時加入引用計數（`s_PhysicsInstances`），防止多個 `Physics3D` 實例重複注冊 Jolt singleton。`physics_3d.hpp` 中 `jobSystem` 成員型別也從 `Vapor::JoltEnkiJobSystem` 改為基底類別 `JPH::JobSystem`。

**尚待完成**：`tests/physics_safeguard_test.cpp` 已建立骨架，但 Jolt 完整初始化在 CI 上的可行性尚未驗證。

---

## 8. 架構強項

| 強項 | 說明 |
|------|------|
| 清晰的 Library / App 分離 | Vapor 不依賴 Vaporware，可作為獨立函式庫使用 |
| Graphics API 抽象 | `Renderer` 介面隔離 Metal/Vulkan，使用者不接觸 API 細節 |
| Thread-safe Resource Loading | `ResourceManager` + `TaskScheduler` 設計正確，支援安全的非同步載入 |
| 物理 / 場景圖分離 | Physics3D 是獨立 singleton，Scene 只是資料；互動透過 `process(scene)` 驅動 |
| Handle-based API | 避免了指標暴露到公開 API，方便未來替換底層實作 |
| Shape Cache | 相同尺寸形狀自動共用，避免 Jolt 重複配置 |
| 模組化 Graphics Headers | `graphics.hpp` 已拆分為 5 個聚焦 sub-header，降低重新編譯範圍 |
| Type-safe GPU Handles | `GPUHandle<Tag>` 模板在 graphics 層提供編譯期 handle 型別安全 |
| 自動化測試 + CI | Catch2 v3 + GitHub Actions；68 個測試案例在每次 PR 自動執行 |
