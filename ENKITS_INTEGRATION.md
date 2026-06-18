# enkiTS 整合文檔

## 概述

本次整合將 enkiTS 任務調度器集成到 Project Vapor 引擎中，實現了統一的多執行緒任務管理系統，支持：
- 異步資源加載
- Jolt 物理引擎的多執行緒模擬
- 為未來的多執行緒渲染做準備

## 架構設計

### 核心組件

#### 1. TaskScheduler (`task_scheduler.hpp/cpp`)
enkiTS 的包裝類，提供簡化的任務調度接口。

**特性：**
- 自動檢測並使用硬體執行緒數
- 支援 Lambda 函數作為任務
- 統一的任務等待機制

**使用示例：**
```cpp
TaskScheduler scheduler;
scheduler.init(); // 自動使用所有可用執行緒

scheduler.submitTask([]() {
    // 在工作執行緒上執行的任務
    loadTexture("texture.png");
});

scheduler.waitForAll(); // 等待所有任務完成
```

#### 2. AsyncAssetLoader (`async_asset_loader.hpp/cpp`)
異步資源加載器，支持並行加載多種資源類型。

**支援的資源類型：**
- Image（圖片）
- GLTF/GLB 場景
- 未來將支持：Mesh, Material, Shader 等

**使用示例：**
```cpp
AsyncAssetLoader loader(taskScheduler);

// 異步加載圖片
auto asyncImage = loader.loadImageAsync("texture.png",
    [](std::shared_ptr<Image> image) {
        // 加載完成回調
        fmt::print("Image loaded: {}x{}\n", image->width, image->height);
    });

// 異步加載場景
auto asyncScene = loader.loadGLTFAsync("model.gltf", true,
    [](std::shared_ptr<Scene> scene) {
        // 場景加載完成
        fmt::print("Scene loaded with {} nodes\n", scene->nodes.size());
    });

// 檢查加載狀態
if (asyncImage->isReady()) {
    auto image = asyncImage->data;
    // 使用已加載的圖片
}

// 等待所有加載完成
loader.waitForAll();
```

#### 3. JoltEnkiJobSystem (`jolt_enki_job_system.hpp/cpp`)
Jolt Physics 的 JobSystem 實現，使用 enkiTS 作為底層調度器。

**特性：**
- 將 Jolt 的物理模擬任務分發到 enkiTS 執行緒池
- 避免創建額外的執行緒池，統一管理所有執行緒
- 支持 Jolt 的所有並行化特性（碰撞檢測、約束求解等）

**整合方式：**
```cpp
TaskScheduler scheduler;
scheduler.init();

JoltEnkiJobSystem joltJobSystem(scheduler);

// 初始化物理系統時傳入
Physics3D physics;
physics.init(&joltJobSystem);
```

#### 4. EngineCore (`engine_core.hpp/cpp`)
引擎核心類，統一管理所有子系統。

**職責：**
- 創建並管理全局 TaskScheduler
- 提供 AsyncAssetLoader 實例
- 提供 JoltEnkiJobSystem 實例供物理系統使用
- 協調各子系統的生命週期

**使用示例：**
```cpp
EngineCore engineCore;
engineCore.init(); // 初始化所有子系統

// 獲取子系統
auto& assetLoader = engineCore.getAssetLoader();
auto* joltJobSystem = engineCore.getJoltJobSystem();

// 在主循環中更新
engineCore.update(deltaTime);

// 關閉時清理
engineCore.shutdown();
```

## 文件變更

### 新增文件
- `Vapor/task_scheduler.hpp/cpp` - enkiTS 包裝
- `Vapor/async_asset_loader.hpp/cpp` - 異步資源加載器
- `Vapor/jolt_enki_job_system.hpp/cpp` - Jolt 任務系統適配器
- `Vapor/engine_core.hpp/cpp` - 引擎核心管理類

### 修改文件
- `vcpkg.json` - 添加 enkiTS 依賴
- `Vapor/CMakeLists.txt` - 添加新源文件和鏈接 enkiTS
- `Vapor/physics_3d.hpp/cpp` - 修改為使用 JoltEnkiJobSystem
- `Vapor/main.cpp` - 整合 EngineCore 和新的初始化流程

## 使用方法

### 基本初始化

```cpp
#include "engine_core.hpp"

int main() {
    // 創建引擎核心
    auto engineCore = std::make_unique<Vapor::EngineCore>();
    engineCore->init(); // 自動檢測執行緒數

    // 初始化物理系統（使用共享的任務調度器）
    Physics3D physics;
    physics.init(engineCore->getJoltJobSystem());

    // 異步加載資源
    auto& assetLoader = engineCore->getAssetLoader();
    auto scene = assetLoader.loadGLTFAsync("model.gltf");

    // 主循環
    while (running) {
        engineCore->update(deltaTime);
        physics.process(scene, deltaTime);
        renderer->draw(scene, camera);
    }

    // 清理
    physics.deinit();
    engineCore->shutdown();

    return 0;
}
```

### 異步資源加載示例

```cpp
// 並行加載多個資源
auto& loader = engineCore->getAssetLoader();

std::vector<std::shared_ptr<AsyncImage>> images;
for (const auto& texturePath : texturePaths) {
    images.push_back(loader.loadImageAsync(texturePath));
}

// 等待所有資源加載完成
loader.waitForAll();

// 檢查加載結果
for (auto& asyncImg : images) {
    if (asyncImg->isReady()) {
        // 使用已加載的圖片
        renderer->uploadTexture(asyncImg->data);
    } else if (asyncImg->isFailed()) {
        fmt::print("Failed to load: {}\n", asyncImg->error);
    }
}
```

## 性能優勢

1. **統一的執行緒管理**
   - 所有子系統共享同一個執行緒池
   - 避免執行緒過多導致的上下文切換開銷
   - 更好的 CPU 緩存利用率

2. **並行資源加載**
   - 多個資源可以同時加載（I/O 並行）
   - 圖片解碼、網格處理等計算密集操作並行化
   - 減少場景加載時間

3. **物理模擬加速**
   - Jolt Physics 充分利用多核心
   - 碰撞檢測、約束求解等並行執行
   - 支持更複雜的物理場景

4. **為多執行緒渲染準備**
   - 任務調度器可用於渲染命令錄製
   - 支持多執行緒的資源上傳
   - 未來可並行化 Culling、Shadow Map 等操作

## 未來擴展

### 計劃中的功能

1. **完全異步的資源管理**
   - 資源流式加載（Streaming）
   - LOD 動態切換
   - 背景資源預加載

2. **多執行緒渲染**
   - 並行渲染命令錄製
   - 多執行緒 Culling
   - 並行 Shadow Map 生成

3. **作業依賴圖**
   - 建立任務之間的依賴關係
   - 自動調度複雜的作業流程
   - 優化執行順序

4. **性能分析工具**
   - 任務執行時間追蹤
   - 執行緒利用率監控
   - 瓶頸分析

## 技術細節

### 執行緒安全

所有公共 API 都是執行緒安全的：
- `AsyncAssetLoader` 使用 `std::mutex` 保護共享狀態
- `TaskScheduler` 本身是執行緒安全的
- 資源加載完成後的回調在工作執行緒上執行

### 記憶體管理

- 使用 `std::shared_ptr` 管理資源生命週期
- 異步任務通過 Lambda 捕獲保持引用計數
- EngineCore 管理子系統的所有權

### 錯誤處理

- 資源加載失敗會設置 `AssetLoadStatus::Failed`
- 錯誤訊息存儲在 `AsyncAsset::error` 字段
- 不會拋出異常，保證執行緒安全

## 依賴項

- **enkiTS**: 輕量級的 C++ 任務調度器
- **Jolt Physics**: 物理引擎（已集成）
- **fmt**: 格式化輸出庫
- **C++20**: 需要 C++20 標準支持

## 編譯要求

```bash
# 使用 vcpkg 安裝依賴
vcpkg install enkits

# CMake 配置
cmake -B build -DCMAKE_TOOLCHAIN_FILE=vcpkg/scripts/buildsystems/vcpkg.cmake

# 編譯
cmake --build build
```

## 授權

本整合遵循 Project Vapor 的 MIT 授權。
