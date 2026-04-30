# Phoenix Workflow Phase 2 — 交接摘要

我已經完成了 Phase 1 的盤點，並在嘗試為核心模組（物理與資源管理）建立 特徵化測試 (Characterization
Tests) 時遇到了技術瓶頸。

以下是目前的狀態摘要，供下一位 Agent 接手：


## 1. 目前進度 (What has been done)

* Phase 1 (Discovery): 已確認 ARCHITECTURE.md 與 FEATURES.md 內容完整，涵蓋了引擎核心與渲染系統。
* Phase 2 (Safeguarding):
    * 新增測試目標: 在 tests/CMakeLists.txt 中新增了 test_physics 與 test_resource_manager。
    * 資源管理測試: tests/resource_manager_safeguard_test.cpp 已建立並通過測試（含同步/非同步載入）。
    * 物理系統修正: 發現 Physics3D::init 重複呼叫會導致 Jolt Physics 全域狀態崩潰，已引入
      s_PhysicsInstances 靜態計數器保護全域初始化邏輯。


## 2. 當前瓶頸 (The Current Roadblock)

* Physics Test Crash: test_physics 在 macOS 上執行時會觸發 Trace/BPT trap:
  5（通常是斷言失敗或非法指令）。
* 定位分析:
    * 崩潰點發生在 physics.addBody() 之後。
    * 懷疑對象: 自定義的 JoltEnkiJobSystem（將 Jolt 任務橋接至 enkiTS）可能存在實作缺陷（如 Barrier
      處理或任務生命週期管理）。
    * 我剛剛將 Physics3D 中的 jobSystem 型別從具體類別改為基底類別 JPH::JobSystem，以便進行替換測試。


## 3. 下一步行動 (Next Steps)

下一位 Agent 應從這裡開始：

1. 驗證崩潰原因: 繼續執行我剛寫好的計畫——將 Physics3D::init 暫時切換為 Jolt 內建的
   JPH::JobSystemThreadPool。
    * 如果不再崩潰: 則確定問題在 Vapor/src/jolt_enki_job_system.cpp 的橋接邏輯。
    * 如果持續崩潰: 問題可能出在物理層 (Layers) 或過濾器 (Filters) 的初始化設定。
2. 修復 JoltEnkiJobSystem: 若確定是橋接問題，需檢查其 new LambdaTask 是否造成洩漏，以及 WaitForJobs
   是否正確同步。
3. 完成 Phase 2: 確保所有測試（含 Action, Scene, Camera, Physics, Resource）在 CI
   環境（或本地測試集）穩定通過。
4. 進入 Phase 3: 處理盤點中發現的技術債（例如 TaskScheduler 中潛在的記憶體洩漏）。


## 待接手檔案清單

* Vapor/src/physics_3d.cpp: 正在修改 init 以測試不同的 JobSystem。
* tests/physics_safeguard_test.cpp: 物理系統的特徵化測試腳本。
* Vapor/include/Vapor/physics_3d.hpp: 已將 jobSystem 抽象化。


您可以直接告訴下一個 Agent：「接手 Phoenix Workflow Phase 2，解決 test_physics 的 Trace/BPT trap: 5
崩潰問題。」
