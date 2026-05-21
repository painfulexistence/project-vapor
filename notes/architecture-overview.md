# Project Vapor — Architecture Deep Dive

> 專案類型：Game Engine (C++20) + Demo Application
> 分析日期：2026-04-29
> 背景：Engine + Gameplay 完整專案。Vapor 是引擎函式庫；Vaporware 是示範應用程式。
> Gameplay code 與引擎透過公開 header API 解耦，但 demo 的 main.cpp 非常龐大（25k LOC）。
> 核心系統：Renderer（Metal/Vulkan）、Physics（Jolt）、Audio（miniaudio）、ECS（EnTT，demo 層）。

---

## 階段 1：EngineCore — 子系統協調者

**這步解決什麼問題？**
引擎有六個獨立子系統（TaskScheduler、ResourceManager、ActionManager、InputManager、AudioManager、RmlUiManager）；EngineCore 是唯一的擁有者，防止使用者管理複雜的初始化順序依賴。

**為什麼選 singleton 而非依賴注入？**
替代方案：傳遞 EngineCore reference 給所有系統。Tradeoff：singleton 讓任何地方都能用 `EngineCore::Get()` 存取，犧牲可測試性（難以在測試中替換）。另一個替代：ECS World 物件（Bevy 風格）——更靈活但複雜度更高。

**關鍵程式碼走讀**
`engine_core.hpp:14-90`：EngineCore 擁有六個 `unique_ptr` 子系統。`init(numThreads)` 建立 enkiTS thread pool；`update(dt)` 依序推進 ActionManager 和 AudioManager。注意：Physics3D **不是** EngineCore 的子系統——它是另一個獨立 singleton（`Physics3D::Get()`）。這個設計決策造成使用者必須自己管理 Physics3D 的生命週期。

---

## 階段 2：Scene Graph — 延遲世界 Transform 計算

**這步解決什麼問題？**
3D 場景中每個物件的世界位置由其所有祖先節點的 transform 組合而來。如果每次移動父節點就立即更新所有子孫，會有大量冗餘計算。`isTransformDirty` flag 推遲計算到真正需要時（`Scene::update()`）。

**為什麼用 mat4 而非分開的 pos/rot/scale？**
替代方案：儲存 position、quaternion、scale 三個欄位（Unity 風格）。Tradeoff：mat4 直接儲存是 GPU-ready 的，避免渲染時的「TRS rebuild」，但每次讀取 position/scale 需要反解矩陣（`scene.hpp:43-86` 的 get* 系列）。特別注意 `getLocalScale()` 用向量長度反算 scale——這在有非均勻縮放的父節點存在時會不精確。

**關鍵程式碼走讀**
`scene.hpp:25-165`：Node 持有 `localTransform`（使用者設定）和 `worldTransform`（引擎計算）。`setLocalPosition`（:88-93）重新組裝 TRS matrix：先讀出現有 rotation/scale，再用 `glm::translate * glm::mat4_cast * glm::scale` 重組——三次矩陣運算才設定一個位置。

---

## 階段 3：Renderer 抽象 — Graphics API 隔離

**這步解決什麼問題？**
Metal（macOS）和 Vulkan（跨平台）API 風格差異巨大；如果直接在遊戲邏輯裡呼叫 Metal/Vulkan，移植就要重寫所有上層程式碼。`Renderer` 純虛介面（`renderer.hpp:26-133`）隔離了兩者。

**為什麼不用 render graph？**
現有設計是「固定 pass 順序」（Pre-pass → Cluster → PBR → Sky → Water → Volumetric → Post → UI）。替代方案：Frame Graph / Render Graph（Frostbite 風格）讓 pass 依賴關係自動排程，更彈性。Tradeoff：Render Graph 複雜度更高，對一個學習型專案來說是 overengineering；但固定順序讓新增 pass 需要改兩個後端（Metal + Vulkan），且沒有共同 pass 介面。

**關鍵程式碼走讀**
`renderer.hpp:135-147`：factory function `createRenderer(GraphicsBackend)` 在執行時選擇後端。`renderer_metal.hpp`（562 LOC）forward-declare 了 16 個 render pass struct，顯示 Metal 後端的「pass 爬行」現象——每個視覺 feature 就增加一個 pass，無統一抽象。

---

## 階段 4：Physics3D — Jolt 整合與形狀快取

**這步解決什麼問題？**
物理模擬需要固定時步（60Hz）而渲染幀率可變；Physics3D 用 `timeAccum`（`:262`）累積殘差，每幀可能 step 0 次或多次，渲染用 `getInterpolationAlpha()` 插值，避免視覺抖動。

**Shape Cache 的意義**
`physics_3d.hpp:225-241`：`ShapeDesc` 用 type + dimensions 作為 key。Jolt 的 Shape 物件是引用計數的，建立相同形狀的 body 自動共用 Shape——對大量同類型物件（例如 2000 個相同大小的箱子）有顯著效能差異。

**關鍵程式碼走讀**
`physics_3d.hpp:86`：`init(TaskScheduler&, DebugDraw*)` — 注意 Physics3D 借用了 EngineCore 的 TaskScheduler（不擁有），用它建立 `JoltEnkiJobSystem`。這個設計讓 Jolt 的物理任務和資源載入任務共用同一個 thread pool。潛在問題：大型場景載入時可能與物理模擬爭搶執行緒。

---

## 階段 5：ResourceManager — Future-like 非同步載入

**這步解決什麼問題？**
場景載入（GLTF）可能耗時數秒，如果在主執行緒同步載入會造成遊戲卡頓。`Resource<T>` 是 future-like 包裝：主執行緒可以提交載入請求，繼續執行其他邏輯，稍後再 `get()`（阻塞）或 `tryGet()`（非阻塞）取得結果。

**為什麼不用 std::future？**
替代方案：`std::promise<T>` + `std::future<T>`。Tradeoff：`std::future` 不支援 completion callback（`Resource<T>::setCallback`）；自訂實作可以在同一個物件上同時支援 polling（`isReady()`）和 callback。代價是多寫了一套 mutex + condition_variable。

**關鍵程式碼走讀**
`resource_manager.hpp:43-50`：`get()` 在 Loading 狀態下 `m_cv.wait(lock)` 阻塞，Loading → Ready 時 `setData()` 呼叫 `m_cv.notify_all()`。注意 callback 在 `setData()` 內、鎖外呼叫（`:106-111`）——避免 callback 內呼叫 `get()` 造成死鎖，設計正確。

---

# Quiz

## 階段 1：EngineCore

**Q1 (概念):** Physics3D 不是 EngineCore 的子系統，而是獨立 singleton。這個設計決策的主要缺點是什麼？

> 你的答案：
>
>

**Q2 (改需求):** 如果你要讓引擎支援多個獨立的 Physics3D 實例（例如兩個物理隔離的場景同時運行），需要改變什麼？

> 你的答案：
>
>

**Q3 (debug):** 使用者回報「AudioManager 在 EngineCore::update 之後呼叫時有時不觸發 finish callback」。最可能的原因是什麼？

> 你的答案：
>
>

**Q4 (設計決策):** EngineCore 使用 singleton pattern 而非 dependency injection。在什麼情況下這個決策會成為最大的痛點？你會如何遷移？

> 你的答案：
>
>

---

## 階段 2：Scene Graph

**Q1 (概念):** `getLocalScale()` 用 `glm::length(glm::vec3(localTransform[0]))` 反算 scale。在什麼情況下這個值會不正確？

> 你的答案：
>
>

**Q2 (改需求):** 如果你要支援「非均勻 shear transform」（即父節點有 skew），現有的 TRS decompose 方法需要怎麼改？

> 你的答案：
>
>

**Q3 (debug):** 一個物理物件在場景中的視覺位置和碰撞體位置有 1 幀的延遲。最可能的原因是什麼？

> 你的答案：
>
>

**Q4 (設計決策):** Node 用 `shared_ptr` 管理子節點。如果場景有 10 萬個節點，這個設計的效能瓶頸在哪裡？替代方案是什麼？

> 你的答案：
>
>

---

## 階段 3：Renderer 抽象

**Q1 (概念):** `renderer.hpp` 的 2D/3D batch API（`drawQuad2D`、`drawLine3D` 等）全部是 virtual 函式，預設實作為空。這個設計有什麼潛在問題？

> 你的答案：
>
>

**Q2 (改需求):** 如果你要新增一個「輪廓描邊（outline）」的 render pass，在現有架構下需要修改哪些檔案？在 Render Graph 架構下呢？

> 你的答案：
>
>

**Q3 (debug):** Vulkan 後端的某個視覺效果（例如 Bloom）在 macOS 上正常但在 Linux 上消失。如何快速定位是後端實作問題還是 pass 順序問題？

> 你的答案：
>
>

**Q4 (設計決策):** 現有的固定 pass 順序 vs. Render Graph。對這個專案來說，從固定 pass 遷移到 Render Graph 的收益是否值得成本？列出你的判斷標準。

> 你的答案：
>
>

---

## 階段 4：Physics3D

**Q1 (概念):** `getInterpolationAlpha()` 回傳 `timeAccum / FIXED_TIME_STEP`。這個值在什麼情況下會大於 1.0？為什麼需要處理這種情況？

> 你的答案：
>
>

**Q2 (改需求):** 如果你要支援「子彈時間」（物理以 0.1x 速度運行但渲染正常），需要修改 `Physics3D::process()` 的什麼部分？

> 你的答案：
>
>

**Q3 (debug):** 使用者回報「在大型場景載入完成的同時，物理模擬會短暫卡頓」。根據架構，最可能的原因是什麼？

> 你的答案：
>
>

**Q4 (設計決策):** Shape Cache 用 `epsilonEqual(0.001f)` 比較 float dimensions。這個精度選擇有什麼邊界情況？在什麼場景下會產生 bug？

> 你的答案：
>
>

---

## 階段 5：ResourceManager

**Q1 (概念):** `Resource<T>::setData()` 在釋放 mutex 後才呼叫 callback。如果 callback 內部又呼叫 `resource->get()`，會發生什麼？

> 你的答案：
>
>

**Q2 (改需求):** 如果你要支援「資源熱重載」（runtime 替換已載入的貼圖），現有的 `ResourceCache` 需要什麼改動？

> 你的答案：
>
>

**Q3 (debug):** 使用者回報「同一個貼圖路徑被載入了兩次（記憶體中有兩份）」。根據 `ResourceCache` 的實作，最可能的原因是什麼？

> 你的答案：
>
>

**Q4 (設計決策):** 現有的 `Resource<T>` 是 future-like 但不支援取消（cancel）。如果要加入取消功能，最大的設計挑戰是什麼？

> 你的答案：
>
>

---

# 參考解答

<details>
<summary>點擊展開</summary>

## 階段 1：EngineCore

**A1:** Physics3D 是獨立 singleton，使用者必須在 EngineCore 之外自行建立並管理它的生命週期（init/deinit 順序）。如果 Physics3D 在 EngineCore 析構後才被銷毀，TaskScheduler 已釋放但 JoltEnkiJobSystem 還在使用它，會造成 dangling reference crash。

**A2:** 移除 `Physics3D::_instance` singleton；改為讓使用者明確建立 `Physics3D` 物件並傳入 `Scene::process()` 或 `EngineCore::update()`。核心挑戰是 `Node` 直接持有 `BodyHandle`，而 BodyHandle 的有效性綁定到特定 Physics3D 實例——需要讓 Node 知道它屬於哪個 physics world。

**A3:** AudioManager 的 finish callback 被放進 `m_pendingCallbacks` 佇列，在 `AudioManager::update(deltaTime)` 中才真正呼叫。如果 update 被跳過（例如主迴圈條件分支），callback 就不會觸發。

**A4:** 單元測試時無法替換 mock 的 AudioManager 或 ResourceManager——所有測試都會觸發真實的檔案系統和音訊裝置。遷移方向：讓 EngineCore 接受介面指標（依賴注入），在測試中傳入 mock 實作。

---

## 階段 2：Scene Graph

**A1:** 當父節點有非均勻 scale 且子節點有旋轉時，世界矩陣的 column 向量長度不再等於 local scale——`glm::length(worldTransform[0])` 會錯誤地混入父節點 scale。`getLocalScale()` 用 `localTransform` 計算理論上正確，但如果 localTransform 本身被直接寫入（`setLocalTransform`）而不經過 TRS 組裝，就無法保證 column 向量正交。

**A2:** Shear transform 無法用 TRS（Translation × Rotation × Scale）分解——需要儲存完整的 4×4 matrix 而非分離的 pos/rot/scale。現有的 `setLocalPosition`、`setLocalRotation`、`setLocalScale` 系列 setter 都依賴 TRS 假設，全部需要廢棄或加上「不支援 shear」的斷言。

**A3:** 物理模擬在 `Physics3D::process()` 更新 Node 的 localTransform，然後 `Scene::update()` 計算 worldTransform。如果渲染在 Scene::update 之後讀取 worldTransform，應該是同幀資料。1 幀延遲通常是因為物理 step 在 Scene::update 之後執行，導致 worldTransform 比物理 body 慢一幀。

**A4:** `shared_ptr` 的引用計數是原子操作，10 萬節點的樹狀遍歷會觸發大量 cache miss（指標追蹤）和原子操作（計數更新）。替代方案：Data-Oriented Design，將 Node 資料以 SoA（Structure of Arrays）存放在連續記憶體，用 index 而非指標引用父子關係。

---

## 階段 3：Renderer 抽象

**A1:** 所有 batch API 都是 virtual 函式，每次呼叫（例如繪製 1000 個 UI 元素）都有 virtual dispatch 開銷。更重要的是：預設實作為空，如果某個後端忘記實作某個方法，編譯器不會報錯，只是默默地畫不出來——這是一個沉默的 bug 陷阱。

**A2:** 現有架構：需要在 `renderer.hpp` 加虛函式宣告、在 `renderer_metal.cpp` 和 `renderer_vulkan.cpp` 各加實作、在兩個後端的 render pass 序列中插入正確位置。Render Graph 架構：只需定義新 pass 的輸入（depth buffer）和輸出（stencil/color），Graph 自動計算排程和資源 barrier。

**A3:** 先在兩個後端各加 `fmt::print` 或 Tracy marker 確認 Bloom pass 是否被執行到。如果 Metal 執行到但 Vulkan 沒有，是後端實作問題。如果兩者都執行到但 Vulkan 輸出是黑色，是 shader 或資源格式問題（SPIR-V 轉換錯誤、image layout 問題）。

**A4:** 對這個學習型專案，Render Graph 的成本（重寫整個渲染後端、學習 DAG 依賴排程）超過收益（pass 複用、自動資源 barrier）。判斷標準：當 pass 數量超過 20 個且需要動態啟用/停用 pass（例如 ray tracing toggle），遷移才值得。

---

## 階段 4：Physics3D

**A1:** 當單幀 delta time 超過 `FIXED_TIME_STEP`（16.67ms）時，`timeAccum` 可能在還沒 step 完就超過 FIXED_TIME_STEP——但 `getInterpolationAlpha()` 在 step 之後才被呼叫，此時 `timeAccum` 是殘差，理論上應在 [0, FIXED_TIME_STEP)。若值 > 1.0，表示 step 迴圈退出條件有 bug，或者 `dt` 傳入了錯誤的值（例如第一幀傳入了啟動時間差）。

**A2:** 加入 `timeScale` 乘數：`process()` 內將傳入的 `dt` 乘以 `timeScale` 再累積到 `timeAccum`。注意：Jolt 的 fixed step 大小（1/60s）不變，只是每個遊戲幀推進的物理時間縮短。CharacterController 和 VehicleController 的 `dt` 參數也需要乘以 `timeScale`。

**A3:** Physics3D 和 ResourceManager 共用同一個 enkiTS thread pool（`TaskScheduler`）。大型場景載入會提交大量 IO + parse 任務，佔用所有 worker thread，Jolt 的物理任務被排在隊伍後面無法及時執行，造成物理 step 時間超出預算。

**A4:** 兩個 shape dimensions 相差不到 0.001f（例如 1.000 和 1.0005）會被當成同一個 shape 共用。如果遊戲中有精度要求很高的堆疊（例如精確的建築模擬），形狀尺寸被 snap 到錯誤的 cached shape 會造成微小穿透。0.001f 對遊戲來說通常夠用，但應該有文件說明這個精度假設。

---

## 階段 5：ResourceManager

**A1:** `setData()` 在釋放 mutex 後呼叫 callback，callback 內部呼叫 `get()` 時，`m_state` 已經是 `Ready`，`get()` 內的 while loop 條件 `m_state == Loading` 為 false，直接回傳 `m_data`，不會死鎖。設計正確，這是刻意將 callback 移到 lock 外的原因（`:106-111` 的註釋說明了這點）。

**A2:** 需要讓 `ResourceCache` 支援「invalidate」：標記某個路徑的快取為 Stale，下次請求時重新載入（而非回傳舊版）。挑戰：已經持有 `shared_ptr<Resource<T>>` 的使用者不會自動收到通知。需要加入觀察者機制（observer pattern）讓使用者訂閱資源更新事件。

**A3:** `ResourceCache::put()` 在 `put` 前沒有再次檢查 cache（check-then-act race condition）：兩個執行緒同時呼叫 `loadImage("same.png")`，第一個執行緒在 `get()` 回傳 nullptr 後、`put()` 之前被搶先，第二個執行緒也取得 nullptr 並建立第二個 Resource 物件，兩者都完成載入後各自 `put()`，後者覆蓋前者，但前者的 shared_ptr 仍然存在——記憶體中暫時有兩份資料。修正：在 `put()` 內再次確認 cache 不存在才寫入（double-checked locking）。

**A4:** 最大挑戰是「已提交到 worker thread 的任務無法取消」——enkiTS 的 `ITaskSet` 一旦提交就必須執行完畢。要支援取消，需要：(1) 在 loader 函式內定期檢查取消 flag；(2) `setFailed()` 以「使用者取消」為原因觸發 cv.notify_all；(3) 所有等待 `get()` 的呼叫者需要能區分「載入失敗」和「載入取消」。

</details>
