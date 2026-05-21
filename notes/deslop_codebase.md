# Deslop Report: Full Codebase (All Mode)

**Mode**: All
**Risk Level**: medium (OVR_001 dead-code removals are irreversible; verify no external caller before deleting)
**Total Findings**: 21

---

## Findings

### physics_3d.cpp

- [ ] `VRB_001` `physics_3d.cpp` L279：comment-out 的舊 jobSystem 行殘留
  ```cpp
  // jobSystem = std::make_unique<Vapor::JoltEnkiJobSystem>(taskScheduler, 2048);
  jobSystem = std::make_unique<JPH::JobSystemThreadPool>(...);
  ```
  建議：刪除 L279 的 comment-out 行；切換決策已在 ADR-005 記錄。

- [ ] `OVR_001` `physics_3d.cpp` L347–352：整個 `if (s_PhysicsInstances == 0)` 區塊為死碼
  ```cpp
  if (s_PhysicsInstances == 0) {
      // Optional: Keep Jolt types registered...
      // JPH::UnregisterTypes();
      // delete JPH::Factory::sInstance;
      // JPH::Factory::sInstance = nullptr;
  }
  ```
  建議：刪除整個 if 區塊，保留 `s_PhysicsInstances--` 即可。

- [ ] `OVR_001` `physics_3d.cpp` L565–580：`// debug output` debugCounter 區塊 comment-out 死碼
  ```cpp
  // static int debugCounter = 0;
  // if (++debugCounter % 60 == 0) { // print every 60 frames
  //     for (auto& node : scene->nodes) { ... }
  // }
  ```
  建議：直接刪除 L565–580。

- [ ] `STR_001` `physics_3d.cpp` L357–581：`Physics3D::process()` 約 224 行，超過 80 行門檻
  可拆解為 `syncSceneToPhysics()`, `stepPhysics()`, `syncPhysicsToScene()`, `syncCharacterControllers()`, `syncVehicleControllers()`, `processEvents()` 六個 private helpers。

- [ ] `STR_003` `physics_3d.cpp` L413, L430, L443, L499, L516：五個 `[&]` 全捕獲遞迴 lambda
  ```cpp
  std::function<void(const std::shared_ptr<Node>&)> storePreviousPositions = [&](...)
  std::function<void(const std::shared_ptr<Node>&)> updateVehicleControllers = [&](...)
  std::function<void(const std::shared_ptr<Node>&)> updateCharacterControllers = [&](...)
  std::function<void(const std::shared_ptr<Node>&)> syncCharacterControllers = [&](...)
  std::function<void(const std::shared_ptr<Node>&)> syncVehicleControllers = [&](...)
  ```
  每次 `process()` 呼叫都重新構造，且每個實際只需捕獲特定成員。
  建議：提升為 private static/member functions，消除 `std::function` 堆積開銷。

- [ ] `NAM_001` `physics_3d.hpp` private members：`broad_phase_layer_interface`, `object_vs_broadphase_layer_filter`, `object_vs_object_layer_filter` 使用 snake_case，與檔案內其他 camelCase 成員（`physicsSystem`, `bodyInterface`, `timeAccum` 等）不一致。
  建議：重命名為 `broadPhaseLayerInterface`, `objectVsBroadphaseLayerFilter`, `objectVsObjectLayerFilter`。

---

### asset_manager.cpp

- [ ] `OVR_001` `asset_manager.cpp` L143–205：63 行完整的舊版 GLTF loader 被 comment-out
  這是被 tinygltf 版本取代前的 cgltf 實作，cgltf 依賴已從 vcpkg.json 移除，無法再啟用。
  建議：直接刪除 L143–205 整個 comment-out 區塊。

- [ ] `OVR_001` `asset_manager.cpp` L420–432：12 行 color 處理 comment-out 死碼
  ```cpp
  if (mesh->hasColor) {
      // const auto& accessor = model.accessors[primitive.attributes.at("COLOR_0")];
      // ... (10 行)
  }
  ```
  空 if 區塊加上被 comment-out 的內容，目前功能為零。
  建議：若 vertex color 無近期實作計畫，刪除整個 if 區塊（L420–433）。若有計畫，改為 `// TODO: implement vertex color (COLOR_0 accessor)`。

---

### mesh_builder.hpp

- [ ] `OVR_001` `mesh_builder.hpp` L11–17：`buildTriforce()` 開頭 7 行 comment-out 的舊 vertex 格式
  ```cpp
  // glm::vec3 verts[6] = { ... };
  // glm::vec2 uvs[6] = { ... };
  ```
  建議：刪除 L11–17 的 comment-out 行。

- [ ] `OVR_001` `mesh_builder.hpp` L38–273：`buildCube()` 內約 235 行 comment-out 的舊 vertex array 寫法
  舊格式使用 `float verts[192]` 平鋪，已用 `std::array<VertexData, 24>` 取代（L274 起），舊版本不可能再啟用。
  建議：刪除 L38–273 之間的所有 comment-out 行（保留 L274 起的 active 實作）。

---

### debug_draw.cpp

- [ ] `VRB_003` `debug_draw.cpp` L74, L100, L204, L238：`const float pi` 在四個函數中重複宣告
  ```cpp
  const float pi = glm::pi<float>(); // addSphere
  const float pi = glm::pi<float>(); // addCapsule
  const float pi = glm::pi<float>(); // addCylinder
  const float pi = glm::pi<float>(); // addCone
  ```
  建議：在檔案頂層加一個 `static constexpr float kPi = glm::pi<float>();`，移除四個函數內的重複宣告。

---

### audio_engine.cpp

- [ ] `VRB_001` `audio_engine.cpp` L11–27：`// ============================================================` section-header 式裝飾性分隔線（至少 2 處）
  這類分隔線只描述 section 結構，不提供動機資訊。
  建議：刪除所有 `// ====` 分隔行及其緊接的 section title 行。

---

### Vaporware/src/systems.hpp

- [ ] `NAM_001` `systems.hpp` L76, L133, L187, L237, L320, L399, L477, L567：8 個 class 的 `static void update` 沒有縮排在 `public:` 內
  ```cpp
  class LightMovementSystem {
  public:
  static void update(...) {   // ← 頂格，未縮排
  ```
  對比正確格式（`CleanupSystem` L28、`CameraSwitchSystem` L147 等）：
  ```cpp
  class CleanupSystem {
  public:
      static void update(...) {  // ← 4-space indent
  ```
  受影響：`LightMovementSystem`, `AutoRotateSystem`, `CameraSystem`, `HUDSystem`, `ScrollTextSystem`, `LetterboxSystem`, `SubtitleSystem`, `ChapterTitleSystem`（均為 T3 重構時從 free function 轉換的 class）。
  建議：為 8 個 class 的函數體補上 4-space indentation，對齊檔案內既有慣例。

- [ ] `NAM_002` `systems.hpp`：timer 參數命名不一致
  - `RmlUIHelpers::tickTimer` (L20) 使用 `dt`
  - 所有 `XSystem::update` 使用 `deltaTime`
  建議：將 `tickTimer` 的參數改為 `deltaTime` 以統一全檔命名。

- [ ] `STR_003` `systems.hpp` L12–13：`RmlUIHelpers` namespace 內的函數宣告為 `static`，在 namespace 內此關鍵字多餘
  ```cpp
  namespace RmlUIHelpers {
      static Rml::ElementDocument* ensureDocument(...) { ... }
      static bool tickTimer(...) { ... }
  }
  ```
  建議：移除兩個 `static` 關鍵字。

---

### Vaporware/src/main.cpp

- [ ] `OVR_001` `main.cpp` L99, L170, L278–279, L419, L565–566（debug output）：多處 comment-out 的死碼行
  - L99：`// ImGui::StyleColorsDark();`
  - L170：`// BodyCreateSystem::update(...)` （BodyCreateSystem 已不再直接呼叫）
  - L278–279：renderer/engineCore resize 佔位符
  - L419：camera warning log
  建議：逐一確認後刪除；L170 尤其安全。

- [ ] `STR_003` `main.cpp` L283, L301：事件 callback lambda `[&]` 全捕獲，實際只需少數變數
  ```cpp
  sdlWindow.onResize([&](int w, int h) { ... }); // only needs windowWidth, windowHeight
  inputHandler.onEvent([&](SDL_Event& e) { ... }); // only needs inputState
  ```
  建議：精確化捕獲列表。

- [ ] `NAM_001` `scene_builder.hpp` L28–29：alignment padding 造成不一致格式
  ```cpp
  res.scene    = scene;    // 多個空格對齊
  res.material = material;
  ```
  vs 其餘賦值無對齊（L33 起）。對齊賦值是選擇性風格，但需全檔一致。
  建議：去掉對齊空格，統一為單空格。

- [ ] `VRB_003` `scene_builder.hpp` L36, L60, L79 等：`.5f` 省略前導零 vs `main.cpp` 的 `0.5f`
  ```cpp
  col.halfSize = glm::vec3(.5f, .5f, .5f);   // scene_builder.hpp
  float quadSize = 20.0f;                     // main.cpp
  ```
  建議：統一使用 `0.5f` 格式（帶前導零），與 main.cpp 及其他檔案一致。

---

### components.hpp

- [ ] `VRB_001` `components.hpp` L18, L22：inline comment 缺少前導空格
  ```cpp
  int lightIndex = -1;// Index into Scene::pointLights  ← 缺 space
  ```
  應為：
  ```cpp
  int lightIndex = -1; // Index into Scene::pointLights
  ```
  建議：在 `//` 前補一個空格（2 處）。

---

## 質檢報告 (L1–L4)

- **L1 (Hard Rule)** ✅：
  - `physics_3d.cpp` L399 的 `// TODO: fix trace trap` 未被觸及。
  - `asset_manager.cpp` 中所有 TODO 均未被標記刪除。
  - `main.cpp` L169 的 `// TODO: migrate to body create system` 未被標記刪除（L170 的死碼行才是刪除對象）。

- **L2 (Consistency)** ✅：
  - NAM_001 建議的重命名均符合各檔案主流慣例。
  - `systems.hpp` indentation 修正依據的是檔案內既有 5 個正確 class 的格式。

- **L3 (Safety)** ✅：
  - 所有 OVR_001 刪除對象均為 comment-out 代碼，無執行路徑。
  - `systems.hpp` indentation 修正為純格式變更，不影響行為。
  - `static` 移除（namespace 內）不影響 linkage 語義。

- **L4 (Master Final)** ✅：
  - `systems.hpp` 的 8 class indentation 問題是 T3 重構留下的機械性殘留，修正後手寫質感明顯提升。
  - `mesh_builder.hpp` 的 235 行死碼仍是最優先清除目標。
  - `scene_builder.hpp` 的 `.5f` 與對齊問題是細節，但會在 code review 時讓讀者停頓。

**總結**：21 個發現，新增 7 個 style inconsistency（主要來自 T3 重構未對齊 indentation、float literal 不一致、comment spacing）。優先序：`systems.hpp` indentation（影響最大，8 個 class）> OVR_001 死碼清除 > 細節格式統一。

---

## 建議執行順序

| 優先 | ID | 檔案 | 風險 | 說明 |
|------|-----|------|------|------|
| 1 | NAM_001 | systems.hpp L76,133,187,237,320,399,477,567 | 低 | 8 class indentation，T3 遺留的格式問題 |
| 2 | OVR_001 | mesh_builder.hpp L38–273 | 低 | 235 行死碼 |
| 3 | OVR_001 | asset_manager.cpp L143–205 | 低 | 63 行死碼，cgltf 依賴已移除 |
| 4 | STR_003 | systems.hpp L12–13 | 低 | namespace 內多餘 `static` |
| 5 | VRB_001 | physics_3d.cpp L279 | 低 | 單行 comment-out |
| 6 | OVR_001 | physics_3d.cpp L347–352, L565–580 | 低 | 空 if 區塊 + debug output 死碼 |
| 7 | VRB_003 | debug_draw.cpp ×4 | 低 | 加一個 static constexpr kPi |
| 8 | OVR_001 | asset_manager.cpp L420–432 | 低 | 空 if + comment-out |
| 9 | VRB_001 | audio_engine.cpp | 低 | 裝飾性分隔行 |
| 10 | NAM_002 | systems.hpp L20 | 低 | tickTimer `dt` → `deltaTime` |
| 11 | VRB_001 | components.hpp L18,22 | 低 | inline comment 補空格 |
| 12 | VRB_003 | scene_builder.hpp | 低 | `.5f` → `0.5f` 前導零 |
| 13 | NAM_001 | scene_builder.hpp L28–29 | 低 | 去掉對齊 padding |
| 14 | NAM_001 | physics_3d.hpp | 中 | snake_case → camelCase 全域重命名 |
| 15 | OVR_001 | main.cpp dead code | 低 | 逐一確認後刪除 |
| 16 | STR_001 | physics_3d.cpp process() | 中 | 拆解長函數（選擇性）|
| 17 | STR_003 | physics_3d.cpp ×5, main.cpp ×2 | 中 | lambda 精確捕獲（選擇性）|
