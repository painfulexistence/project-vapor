# Project Vapor — 嚴格 Code Review 報告

**日期：** 2026-05-27  
**審查範圍：** 全 codebase（Vapor 引擎 + Vaporware demo）  
**分類：** 命名一致性、設計 pattern、新舊系統混用、重複代碼、不良 C++ pattern

---

## 目錄

1. [命名 Pattern 不一致](#1-命名-pattern-不一致)
2. [系統設計 Pattern 不一致](#2-系統設計-pattern-不一致)
3. [新舊系統混合](#3-新舊系統混合)
4. [重複代碼](#4-重複代碼)
5. [不良或老舊的 C++ Pattern](#5-不良或老舊的-c-pattern)
6. [嚴重性總覽](#6-嚴重性總覽)

---

## 1. 命名 Pattern 不一致

### 1.1 Enum 值——三種不同命名風格並存

**位置：** `renderer.hpp`

```cpp
// PascalCase — GraphicsBackend / RenderPath
enum class GraphicsBackend { Metal, Vulkan };          // L21
enum class RenderPath { Forward, Deferred };            // L23

// UPPER_SNAKE_CASE — BufferUsage / RenderTargetUsage
enum class BufferUsage { VERTEX, INDEX, UNIFORM, STORAGE, COPY_SRC, COPY_DST }; // L25
enum class RenderTargetUsage { COLOR_MSAA, COLOR, DEPTH_MSAA, DEPTH }; // L27
```

**位置：** `graphics_gpu_structs.hpp`

```cpp
// UPPER_SNAKE_CASE — 第三個 file 繼續混用
enum class PrimitiveMode { POINTS, LINES, LINE_STRIP, TRIANGLES, TRIANGLE_STRIP }; // L11
```

**位置：** `physics_3d.hpp`

```cpp
// PascalCase 又回來了
enum class MotionType { Static, Dynamic, Kinematic }; // L42-46
```

**位置：** `graphics_mesh.hpp`

```cpp
// 全大寫但沒有 _SEPARATOR
enum class AlphaMode { OPAQUE, MASK, BLEND }; // L15
```

> **結論：** 同一 codebase 中 enum 值同時存在 `PascalCase`、`UPPER_SNAKE_CASE`、`UPPER_CASE`（無底線）三種風格，且分佈在多個核心 header，無統一規範。

---

### 1.2 Class 命名——唯一的 snake_case 類名

**位置：** `renderer_vulkan.hpp:20`

```cpp
class Renderer_Vulkan final : public Renderer {  // ← 唯一用底線的類名
```

其餘所有類皆為 PascalCase：`EngineCore`、`Physics3D`、`AudioEngine`、`RmlUiManager`、`SceneInspector` 等。  
應改為 `RendererVulkan`。

---

### 1.3 成員變數前綴——四種不同約定並存

| 前綴 | 範例 | 位置 |
|------|------|------|
| 無前綴 | `physicsSystem`, `jobSystem`, `position`, `scale` | `physics_3d.hpp`, `components.hpp` |
| `m_` | `m_idIndex`, `m_definitions`, `m_searchBuf` | `definition.hpp`, `scene_inspector.hpp` |
| `_` (leading) | `_taskScheduler`, `_resourceManager`, `_eye`, `_center` | `engine_core.hpp`, `camera.hpp` |
| `s_` (static) | `s_instance` | `engine_core.hpp:78` |

其中 `engine_core.hpp` 同時出現 `_` 前綴成員（`_taskScheduler`）和 `s_` 前綴靜態成員（`s_instance`），同一個 class 就用了兩種 convention。

`renderer_vulkan.hpp` 幾乎全部成員無前綴，但第 374 行突然出現：
```cpp
std::vector<ScreenshotCallback> m_pendingScreenshotRequests; // L374 ← 唯一有 m_ 前綴的成員
```

---

### 1.4 Boolean 成員命名——`is` 前綴有時有、有時沒有

```cpp
// 有 is 前綴：
bool isActive;            // components.hpp:69
bool isTransformDirty;    // scene.hpp:31
bool isGeometryDirty;     // graphics_mesh.hpp
bool isInitialized;       // engine_core.hpp
bool jumpRequested;       // components.hpp ← 無前綴

// 無 is 前綴：
bool visible;             // components.hpp:36, 143
bool doubleSided;         // graphics_mesh.hpp:30
bool particleSystemEnabled; // renderer_vulkan.hpp:336
```

---

### 1.5 縮寫命名——含義不明確

| 縮寫 | 應改為 | 位置 |
|------|--------|------|
| `rid` | `resourceId` | `graphics_handles.hpp:11` |
| `hdr` | `isHDR` 或 `hdrEnabled` | RenderTextureDesc |
| `Idx` | `Index` | `graphicsFamilyIdx`, `presentFamilyIdx`（`renderer_vulkan.hpp:243-244`）|
| `mieG` | `mieAsymmetry` 或 `mieGFactor` | `graphics_effects.hpp:167` |
| `phys`, `cam`, `fly`, `ref`, `hit` | 完整拼寫 | `systems.hpp` 多處 |
| `dt` | `deltaTime` | `action_manager.hpp` |

---

### 1.6 Descriptor Set 命名——語意空洞

**位置：** `renderer_vulkan.hpp:283-292`

```cpp
VkDescriptorPool set0DescriptorPool;   // 這三個到底是什麼？
VkDescriptorPool set1DescriptorPool;
VkDescriptorPool set2DescriptorPool;
std::vector<VkDescriptorSet> set0s;    // "global" ← 只有 comment 說明
std::vector<VkDescriptorSet> set1s;    // "1 set per material"
std::vector<VkDescriptorSet> set2s;    // 無說明
```

應改為：`globalDescriptorPool`, `materialDescriptorPool`, `globalSets`, `materialSets` 等有意義的名字。

---

### 1.7 方法名後綴不一致——`Idx` vs `Index`

`graphicsFamilyIdx`、`presentFamilyIdx` 用 `Idx`，  
但 `lightIndex`（`physics_3d.hpp`）、`frameIndex`（`components.hpp`）用 `Index`。

---

## 2. 系統設計 Pattern 不一致

### 2.1 Singleton——兩種實作方式

**EngineCore（`engine_core.hpp:78`）：**
```cpp
static EngineCore* s_instance;  // s_ 前綴
static EngineCore* Get() { return s_instance; }
```

**Physics3D（`physics_3d.hpp:92-98`）：**
```cpp
static Physics3D* _instance;   // _ 前綴
static Physics3D* Get() { return _instance; }
```

同樣的 pattern，但成員命名不同（`s_instance` vs `_instance`）。兩者都回傳 raw pointer，呼叫端必須自行判斷 lifetime。

---

### 2.2 Error Handling——三種不同策略並存

| 策略 | 範例 | 位置 |
|------|------|------|
| 拋出 exception | `throw std::runtime_error(...)` | `definition.hpp:185-186` |
| 回傳 `bool` | `bool init(...)` | `audio_engine.hpp:109` |
| Error enum | `enum AudioResult { Error = -1 }` | `audio_engine.hpp:25` |
| 無 error handling | 多數 manager 的方法 | 各處 |

`definition.hpp` 拋出 exception，但其他系統全部用回傳值；engine 的其他地方也幾乎沒有 try/catch。這使得例外安全性完全不可預測。

---

### 2.3 System 函數簽章——引擎層 vs 遊戲層不兼容

**引擎 system（`Vapor/include/Vapor/systems.hpp`）：**
```cpp
static void update(World& world, InputManager& inputManager, float deltaTime);
```

**遊戲 system（`Vaporware/src/systems.hpp`）：**
```cpp
static void update(entt::registry& reg, float deltaTime);
```

遊戲層繞過了引擎的 `World` 抽象，直接用 `entt::registry`，導致兩層的 system 簽章完全不兼容，無法互換。

---

### 2.4 Renderer 的兩個 `draw()` overload 設計矛盾

**位置：** `renderer.hpp:48-49`

```cpp
virtual void draw(std::shared_ptr<Scene> scene, Camera& camera) = 0;
virtual void draw(entt::registry& registry, std::shared_ptr<Scene> scene, Camera& camera) = 0;
```

兩個 draw overload 代表兩種不同的渲染架構（Scene graph 與 ECS），但兩者都是 pure virtual，所有 backend 必須實作兩套。這暗示渲染架構正在從 Scene graph 遷移至 ECS，但遷移尚未完成。

---

### 2.5 Light Reference Component——引擎用泛型、遊戲用具體型別

**引擎（`systems.hpp:279-280`）：**
```cpp
SceneLightReferenceComponent  // 泛型，包含 lightIndex
```

**遊戲（`Vaporware/src/components.hpp:11-17`）：**
```cpp
struct ScenePointLightReferenceComponent { int lightIndex; };
struct SceneDirectionalLightReferenceComponent { int lightIndex; };
```

引擎層和遊戲層對同一概念有兩種不同的型別結構，無法互用。

---

### 2.6 SceneInspector 使用 RmlUI 的方式不一致

`scene_inspector.hpp:34` 存有 `Rml::ElementDocument* doc` 作為成員，  
但 `ui_page_system.hpp` 的 `UIDocumentComponent` 也有自己的 `Rml::ElementDocument* doc = nullptr`。  
兩個不同 class 各自管理 RmlUI document，無共用的所有權策略。

---

## 3. 新舊系統混合

### 3.1 圖片載入——新舊 API 同時存在同一 file

**位置：** `Vaporware/src/main.cpp`

```cpp
// 舊 API（L329）— 靜態函數，同步載入
auto img = AssetManager::loadImage("textures/default_albedo.png");
renderer->createTexture(*img);

// 新 API（L387-391）— 非同步資源管理器
auto texHandle = resourceManager.loadImage(path, Vapor::LoadMode::Async);
auto* tex = texHandle.get();
```

兩種 API 並存，維護上容易誤用舊路徑。

---

### 3.2 Camera——兩套不同的 Camera 抽象

**位置：** `Vaporware/src/main.cpp`

```cpp
// 舊方式（L343-351）— 直接 Camera 物件
Camera rtCamera(glm::vec3(0, 5, 10), glm::vec3(0, 0, 0), 60.0f, aspect);
renderer->draw(scene, rtCamera);

// 新方式（L612-616）— ECS component 模式
Vapor::VirtualCameraComponent camComp;
camComp.setEye(pos);
camComp.setProjectionMatrix(proj);
```

同一個 application 同時使用兩套 camera 系統。

---

### 3.3 Mesh 載入——Staging 舊流程 vs ECS 新流程

**位置：** `Vaporware/src/main.cpp:413-447`

```cpp
// 舊 staging 流程（L415-447）
for (auto& mesh : scene->stagedMeshes) {
    auto e = registry.create();
    registry.emplace<Vapor::TransformComponent>(e);
    // ... 手動轉換 stagedMeshes 為 ECS entity
}
// L443-445 comment: "Clear stagedMeshes... meshes are now ECS entities"
```

`stagedMeshes` 是過渡期的產物，應該已被完全移除，但仍在 production code 中。

---

### 3.4 Legacy CPU Particle——已廢棄但保留

**位置：** `graphics_effects.hpp:249-254`

```cpp
// Legacy CPU particle (kept for compatibility with older systems)
struct Particle {
    glm::vec3 position = glm::vec3(1.0f);
    glm::vec3 velocity = glm::vec3(1.0f);
    glm::vec3 density = glm::vec3(1.0f);  // ← density 是 vec3 沒有意義
};
```

GPU particle system（`GPUParticle` struct）已是新系統，這個舊 CPU struct 應清除。初始值 `glm::vec3(1.0f)` 也是錯的（應為 `0`）。

---

### 3.5 Scene Graph Node 混入 ECS 系統

`scene.hpp` 的 `Node` class 有虛擬方法（`onTriggerEnter`, `onTriggerExit`）代表 OOP scene graph 模式；  
但 `components.hpp` 的 `TriggerVolumeComponent` 用 `std::function<void(entt::entity)>` 回調代表 ECS 模式。  
兩套 trigger 系統並存，行為語意不同。

---

## 4. 重複代碼

### 4.1 Component 在引擎層和遊戲層完全重複定義

**精確重複（應刪除遊戲層版本）：**

| Component | 引擎位置 | 遊戲位置 |
|-----------|----------|----------|
| `FlyCameraComponent` | `components.hpp:72-77` | `Vaporware/components.hpp:71-77` |
| `FollowCameraComponent` | `components.hpp:79-84` | `Vaporware/components.hpp:79-85` |
| `HeldByComponent` | `components.hpp` | `Vaporware/components.hpp:43-47` |

**幾乎重複但有差異（default 值不同）：**

```cpp
// 引擎（components.hpp）
struct GrabberComponent {
    float maxPickupRange = 20.0f;  // ← 20.0
};

// 遊戲（Vaporware/components.hpp:49-52）
struct GrabberComponent {
    float maxPickupRange = 5.0f;   // ← 5.0，不同 default，難以追蹤 bug
};
```

遊戲層 `#include "Vapor/components.hpp"` 後又重新定義相同的 component，造成 shadow definition 問題。

---

### 4.2 Mesh Builder 中三段重複的索引生成迴圈

**位置：** `mesh_builder.hpp:160-215`（`buildCapsule()`）

```cpp
// 同一個 pattern 出現三次（頂部半球、圓柱、底部半球）
for (Uint32 ring = 0; ring < rings; ++ring) {
    for (Uint32 seg = 0; seg < segments; ++seg) {
        Uint32 current = <offset> + ring * (segments + 1) + seg;
        Uint32 next    = current + segments + 1;
        indices.push_back(current);
        indices.push_back(current + 1);
        indices.push_back(next);
        indices.push_back(current + 1);
        indices.push_back(next + 1);
        indices.push_back(next);
    }
}
```

應抽出 helper lambda 或 static 方法。

**位置：** `mesh_builder.hpp:283-331`（`buildCylinder()`）

Top cap（L287-305）與 bottom cap（L312-330）生成邏輯幾乎完全相同，差異只有 y 座標方向。

---

### 4.3 ImGui Inspector 16 個幾乎相同的 registerCustomDrawer

**位置：** `Vaporware/src/main.cpp:36-232`

```cpp
// 同樣的 pattern 重複 16 次
inspector.registerCustomDrawer([](entt::registry& reg, entt::entity e) {
    if (auto* c = reg.try_get<ComponentType>(e)) {
        if (ImGui::CollapsingHeader("Label")) {
            // ImGui 欄位
        }
    }
});
```

應以模板或 macro 抽象化。

---

### 4.4 TransformSystem 在兩層各有實作

- 引擎：`Vapor/include/Vapor/systems.hpp:16-52`（用 `World&`）
- 遊戲：`Vaporware/src/systems.hpp:210-232`（用 `entt::registry&`）

兩套系統共存，且邏輯不同步，遊戲層用 EnTT view 直接操作，引擎層用 pool。

---

## 5. 不良或老舊的 C++ Pattern

### 5.1 Singleton 使用 raw pointer

**位置：** `engine_core.hpp:78`, `physics_3d.hpp:92`

```cpp
static EngineCore* s_instance;  // raw pointer singleton
static Physics3D*  _instance;   // 同上
```

raw pointer singleton 無法確保初始化順序，也無法自動清理。應使用 Meyers singleton 或 `static std::unique_ptr<T>`。

---

### 5.2 大量 `void*` mapped buffer——無型別安全

**位置：** `renderer_vulkan.hpp:306-312`

```cpp
std::vector<void*> cameraDataBuffersMapped;
std::vector<void*> instanceDataBuffersMapped;
std::vector<void*> directionalLightBuffersMapped;
std::vector<void*> pointLightBuffersMapped;
std::vector<void*> particleSimParamsBuffersMapped;
std::vector<void*> particleAttractorBuffersMapped;
```

六個 `void*` vector 沒有任何型別資訊，使用時需要各自 cast，容易出錯。應改為 `std::span<T>` 或型別化的 mapped pointer 封裝。

---

### 5.3 未初始化的成員變數

**位置：** `physics_3d.hpp:297-298`

```cpp
float timeAccum;   // ← 未初始化
Uint32 step;       // ← 未初始化
```

**位置：** `camera.hpp:234-236`

```cpp
glm::mat4 _viewMatrix;                // ← 未初始化
glm::mat4 _projMatrix;                // ← 未初始化
std::array<glm::vec4, 6> _frustumPlanes; // ← 未初始化
```

**位置：** `graphics_mesh.hpp:52`

```cpp
float clearcoatGloss;  // ← 唯一沒有 = value 的成員
```

---

### 5.4 Lazy initialization 沒用 `mutable`

**位置：** `camera.hpp:83-112`

```cpp
// getViewMatrix(), getProjMatrix(), getFrustumPlanes() 都不是 const
// 但它們只是做 lazy computation，邏輯上應該是 const
glm::mat4& getViewMatrix() {   // ← 應該是 const，用 mutable _viewMatrix
    if (_isViewDirty) { ... }  // 修改了 _viewMatrix 和 _isViewDirty
    return _viewMatrix;
}
```

這三個 getter 的設計意圖是 const（只讀取 view），但因為要 lazy init 所以不能加 `const`。應將矩陣成員標記 `mutable`，讓方法能正確標 `const`。

---

### 5.5 缺少 `[[nodiscard]]` 的 getter——20+ 處

關鍵 getter 皆未標記 `[[nodiscard]]`，呼叫端忽略回傳值的 bug 無法在編譯期被發現：

```cpp
// resource_manager.hpp
Resource<T> get();          // L46
bool isReady();             // L62
bool isFailed();            // L67
bool isLoading();           // L72

// input_manager.hpp
bool isHeld(Action);        // L59
bool isPressed(Action);     // L66
bool isReleased(Action);    // L73

// camera.hpp
bool isOrthographic();      // L33
bool isVisible(...);        // L187, L201
glm::mat4& getViewMatrix(); // L83
```

---

### 5.6 缺少 `noexcept` 的簡單查詢方法

```cpp
bool isReady() const;          // resource_manager.hpp — 不會拋例外，應標 noexcept
bool isHeld(Action) const;     // input_manager.hpp
bool isOrthographic() const;   // camera.hpp
glm::vec3 getEye() const;      // camera.hpp
glm::vec3 getForward() const;  // camera.hpp
```

---

### 5.7 `strncpy()` 使用（已廢棄的 C API）

**位置：** `scene_inspector.hpp:181`

```cpp
strncpy(m_searchBuf, ..., sizeof(m_searchBuf));
```

應使用 `std::string` 或 `std::string_view`，或至少用 `std::strncpy` 並確保 null termination。

---

### 5.8 Raw C array 與 `std::array` 在同一 file 中混用

**位置：** `mesh_builder.hpp`

```cpp
Vapor::VertexData verts[6];    // L11 — raw C array
Uint32 indices[6];             // L14 — raw C array

// 但同一 file 其他地方：
std::array<Vapor::VertexData, 24> cubeVerts; // L26 — modern style
```

---

### 5.9 `M_PI` macro 而非 C++20 標準常數

**位置：** `mesh_builder.hpp:92, 99, 122, 147, 254, 289, 314`

```cpp
M_PI * 0.5f    // POSIX macro，非 C++ 標準
2.0f * M_PI    // 重複出現 6+ 次
```

應改為 `std::numbers::pi_v<float>`（C++20）或 `glm::pi<float>()`（已有 glm 依賴）。

---

### 5.10 Signed/unsigned 混合比較

**位置：** `systems.hpp:287-288`

```cpp
if (ref->lightIndex < 0 || ref->lightIndex >= scene->pointLights.size()) {
//  ^^^^^^^^^^^^^^^^                           ^^^^^^^^^^^^^^^^^^^^^^^^^^^
//  int（有號）               vs               size_t（無號）— 隱式轉換
```

應將 `lightIndex` 改為 `size_t` 或 `uint32_t`，或 cast 比較。

---

### 5.11 `std::function` 作為 callback 成員——overhead

**位置：** `components.hpp:127-128`

```cpp
std::function<void(entt::entity)> onEnter;
std::function<void(entt::entity)> onExit;
```

`std::function` 有 type-erasure overhead（heap allocation、虛擬 dispatch）。對於 ECS component（可能有成千上萬個實例），應考慮使用 entity event queue 或 tag component 模式代替。

---

### 5.12 未完成的 stub 實作留在 production code

**位置 1：** `definition.hpp:154-170`

```cpp
void loadFromFile([[maybe_unused]] const std::string& path) { /* TODO */ }
void loadFromJson([[maybe_unused]] const std::string& path) { /* TODO */ }
void loadFromBinary([[maybe_unused]] const std::string& path) { /* TODO */ }
```

**位置 2：** `mesh_builder.hpp:231-234`（`buildCone()`）

```cpp
static std::shared_ptr<Vapor::Mesh> buildCone(...) {
    auto mesh = std::make_shared<Vapor::Mesh>();
    return mesh;   // ← 空 mesh，完全未實作
}
```

**位置 3：** `Vaporware/src/systems.hpp:282-283`（`CharacterMovementSystem`）

```cpp
if (intent.jump) {
    // ← 空的，jump 邏輯未實作
}
```

---

### 5.13 遊戲層污染 global namespace

**位置：** `Vaporware/src/components.hpp`, `Vaporware/src/systems.hpp`

引擎的 `namespace Vapor { ... }` 完整包含所有定義，  
但遊戲層的所有 component 和 system class 都在 global namespace，且同時 include 了引擎 header：

```cpp
#include "Vapor/components.hpp"  // L3 — Vapor namespace
// 然後直接在 global namespace 定義同名（或類似）的 component
struct GrabberComponent { ... };  // ← shadow / 重定義風險
```

---

### 5.14 GPU struct 含 C++ member initializer——概念上錯誤

**位置：** `graphics_effects.hpp:86-108, 121-146, 161-173, 181-212`

```cpp
struct alignas(16) VolumetricFogData {
    float fogDensity = 0.02f;          // ← C++ default value
    float fogHeightFalloff = 0.1f;     // ← 這些值不會送到 GPU
    // ... 30+ 個有 default 的欄位
};
```

GPU buffer struct 的 default value 在 CPU 端初始化時有意義，但讓人誤以為它們已被 upload 到 GPU。正確做法是用獨立的初始化函數，或在 comment 中明確說明這些是 CPU-side 的預設值。

---

### 5.15 `Material` 擁有 `PipelineHandle`——職責分離違反

**位置：** `graphics_mesh.hpp:56`

```cpp
struct Material {
    // ...
    PipelineHandle pipeline;  // ← material 不應該管 pipeline
};
```

Material 是 data（顏色、貼圖、物理屬性），Pipeline 是 runtime 渲染資源。Renderer 應根據 material 的屬性來決定使用哪個 pipeline，而不是讓 material 直接持有 pipeline handle。

---

### 5.16 `ParticleSimulationParams` 內嵌 hardcoded 解析度

**位置：** `graphics_effects.hpp:229`

```cpp
struct alignas(16) ParticleSimulationParams {
    glm::vec2 resolution = glm::vec2(1280.0f, 720.0f);  // ← hardcoded 1280x720
```

GPU struct 內嵌 magic number 解析度，每次視窗大小改變都必須記得更新這個 struct。

---

## 6. 嚴重性總覽

| # | 問題 | 嚴重性 | 位置 |
|---|------|--------|------|
| 1.1 | Enum 值三種命名風格並存 | **HIGH** | `renderer.hpp`, `physics_3d.hpp`, `graphics_mesh.hpp` |
| 1.2 | `Renderer_Vulkan` 用底線命名 class | **MEDIUM** | `renderer_vulkan.hpp:20` |
| 1.3 | 成員前綴四種約定並存 | **HIGH** | 全 codebase |
| 1.4 | Boolean `is` 前綴不一致 | **MEDIUM** | `components.hpp`, `scene.hpp` |
| 1.5 | 縮寫命名含義不明 | **MEDIUM** | `graphics_handles.hpp`, `systems.hpp` |
| 1.6 | Descriptor set 命名語意空洞 | **MEDIUM** | `renderer_vulkan.hpp:283-292` |
| 2.1 | Singleton 兩種實作方式 | **MEDIUM** | `engine_core.hpp`, `physics_3d.hpp` |
| 2.2 | Error handling 三種策略並存 | **HIGH** | `definition.hpp`, `audio_engine.hpp` |
| 2.3 | System 函數簽章引擎/遊戲不兼容 | **HIGH** | `systems.hpp`（兩層）|
| 2.4 | Renderer 兩個 draw() 代表架構未定 | **HIGH** | `renderer.hpp:48-49` |
| 2.5 | Light reference 引擎/遊戲型別不一致 | **MEDIUM** | `systems.hpp`, `Vaporware/components.hpp` |
| 3.1 | 圖片載入新舊 API 並存 | **HIGH** | `main.cpp:329, 387-391` |
| 3.2 | Camera 兩套抽象並存 | **HIGH** | `main.cpp:343-351, 612-616` |
| 3.3 | stagedMeshes 過渡代碼殘留 | **MEDIUM** | `main.cpp:415-447` |
| 3.4 | Legacy CPU Particle struct 殘留 | **MEDIUM** | `graphics_effects.hpp:249-254` |
| 3.5 | Scene graph Node + ECS trigger 兩套並存 | **HIGH** | `scene.hpp`, `components.hpp` |
| 4.1 | Component 引擎/遊戲層重複定義 | **CRITICAL** | `components.hpp`（兩層）|
| 4.2 | Capsule/Cylinder 索引迴圈重複 | **MEDIUM** | `mesh_builder.hpp:160-215, 283-331` |
| 4.3 | 16 個相同 registerCustomDrawer | **LOW** | `main.cpp:36-232` |
| 4.4 | TransformSystem 兩層各有實作 | **HIGH** | `systems.hpp`（兩層）|
| 5.1 | Singleton raw pointer | **HIGH** | `engine_core.hpp:78`, `physics_3d.hpp:92` |
| 5.2 | 六個 `void*` mapped buffer vector | **HIGH** | `renderer_vulkan.hpp:306-312` |
| 5.3 | 未初始化成員 | **HIGH** | `physics_3d.hpp:297-298`, `camera.hpp:234-236` |
| 5.4 | Lazy init 未用 `mutable` | **MEDIUM** | `camera.hpp:83-112` |
| 5.5 | 缺 `[[nodiscard]]` 20+ 處 | **MEDIUM** | `resource_manager.hpp`, `input_manager.hpp`, `camera.hpp` |
| 5.6 | 缺 `noexcept` 於簡單查詢 | **LOW** | 多處 |
| 5.7 | `strncpy()` 已廢棄 C API | **MEDIUM** | `scene_inspector.hpp:181` |
| 5.8 | Raw C array 與 `std::array` 混用 | **MEDIUM** | `mesh_builder.hpp:11, 14` |
| 5.9 | `M_PI` macro 非標準 | **LOW** | `mesh_builder.hpp` 多處 |
| 5.10 | Signed/unsigned 混合比較 | **MEDIUM** | `systems.hpp:287-288` |
| 5.11 | `std::function` ECS callback overhead | **LOW** | `components.hpp:127-128` |
| 5.12 | Stub 實作殘留 production | **HIGH** | `definition.hpp:154-170`, `mesh_builder.hpp:231-234`, `Vaporware/systems.hpp:282-283` |
| 5.13 | 遊戲層污染 global namespace | **HIGH** | `Vaporware/src/` 全部 |
| 5.14 | GPU struct 含 C++ member initializer | **LOW** | `graphics_effects.hpp` |
| 5.15 | `Material` 擁有 `PipelineHandle` | **HIGH** | `graphics_mesh.hpp:56` |
| 5.16 | GPU struct 內嵌 hardcoded 解析度 | **MEDIUM** | `graphics_effects.hpp:229` |

### 嚴重性統計

| 等級 | 數量 |
|------|------|
| CRITICAL | 1 |
| HIGH | 17 |
| MEDIUM | 15 |
| LOW | 5 |
| **合計** | **38** |

---

## 優先改善建議

**立即（CRITICAL / HIGH）：**
1. 刪除遊戲層重複定義的 engine component，改用 engine 版本或明確 namespace 隔離
2. 確立統一的 error handling 策略（建議回傳 `std::expected<T, Error>`）
3. 確立統一的 enum 命名規範（建議全用 PascalCase 值）
4. 統一成員變數前綴（建議全用 `m_`，或全部不用前綴）
5. 初始化所有未初始化的成員（`timeAccum`, `step`, camera matrices）
6. 決定並清除渲染架構中的舊路徑（Scene graph draw 或 ECS draw，選一個）
7. 將 Vaporware 所有代碼包進 `namespace Vaporware`
8. 刪除 `struct Particle`（legacy CPU particle）
9. 刪除或實作所有 stub（`buildCone`, `loadFromFile`, jump handler）

**短期（MEDIUM）：**
1. `Renderer_Vulkan` 改名為 `RendererVulkan`
2. `camera.hpp` getter 加 `mutable` 並標 `const`
3. 所有 getter 加 `[[nodiscard]]`
4. `strncpy` 改用 `std::string` 操作
5. `mesh_builder.hpp` raw array 改 `std::array`，`M_PI` 改 `std::numbers::pi_v<float>`
6. `renderer_vulkan.hpp` descriptor set 改用有意義的命名
7. `Material::pipeline` 移至 renderer 端管理
8. Capsule/Cylinder 重複索引迴圈抽出 helper

**長期（架構）：**
1. 統一 system 參數型別（全用 `World&` 或全用 `entt::registry&`）
2. `void*` mapped buffer 封裝為型別安全的 struct
3. Singleton 改用 Meyers singleton 或 dependency injection
4. Trigger 系統統一為純 ECS event 模式
