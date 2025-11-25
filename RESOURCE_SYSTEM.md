# 現代化資源管理系統

## 概述

本文檔描述 Project Vapor 的現代化資源管理系統，採用模板化設計，統一處理同步和異步資源加載。

## 核心設計

### 1. Resource<T> - 通用資源容器

`Resource<T>` 是一個模板類，封裝任何類型的資源及其加載狀態。

**特性：**
- 線程安全的狀態追蹤
- 支持阻塞和非阻塞訪問
- 自動完成通知機制
- 錯誤處理和報告

**狀態機：**
```
Unloaded → Loading → Ready (成功)
                   → Failed (失敗)
```

**使用示例：**
```cpp
// 創建資源
auto imageResource = std::make_shared<Resource<Image>>("texture.png");

// 檢查狀態
if (imageResource->isReady()) {
    auto image = imageResource->get();  // 立即返回
}

// 阻塞直到加載完成
auto image = imageResource->get();  // 如果正在加載則等待

// 非阻塞訪問
auto image = imageResource->tryGet();  // 可能返回 nullptr

// 設置完成回調
imageResource->setCallback([](std::shared_ptr<Image> img) {
    fmt::print("Image loaded: {}x{}\n", img->width, img->height);
});
```

### 2. ResourceCache<T> - 資源緩存

線程安全的資源緩存，防止重複加載相同資源。

**特性：**
- 基於路徑的緩存鍵
- 自動重用已加載資源
- 內存使用追蹤
- 選擇性清除

**API：**
```cpp
ResourceCache<Image> cache;

// 存儲資源
cache.put("texture.png", imageResource);

// 獲取緩存資源
auto cached = cache.get("texture.png");

// 檢查是否存在
if (cache.contains("texture.png")) {
    // ...
}

// 清除緩存
cache.clear();
```

### 3. ResourceManager - 統一資源管理器

現代化的資源管理器，替代舊的 `AssetManager` 和 `AsyncAssetLoader`。

**核心優勢：**
- ✅ 統一的 API（同步/異步選擇）
- ✅ 自動緩存管理
- ✅ 類型安全的資源訪問
- ✅ 豐富的完成回調
- ✅ 任務調度整合

## API 參考

### 基本加載

#### 圖片加載
```cpp
ResourceManager resourceManager(taskScheduler);

// 異步加載（默認）
auto imageResource = resourceManager.loadImage(
    "texture.png",
    LoadMode::Async,
    [](std::shared_ptr<Image> image) {
        // 加載完成回調
    }
);

// 同步加載（阻塞）
auto imageResource = resourceManager.loadImage(
    "texture.png",
    LoadMode::Sync
);

// 獲取數據
auto image = imageResource->get();  // 阻塞直到加載完成
```

#### 場景加載
```cpp
// 異步加載優化場景
auto sceneResource = resourceManager.loadScene(
    "model.gltf",
    true,  // optimized
    LoadMode::Async,
    [](std::shared_ptr<Scene> scene) {
        fmt::print("Scene has {} nodes\n", scene->nodes.size());
    }
);

// 等待場景準備好
auto scene = sceneResource->get();
```

#### Mesh 加載
```cpp
// 異步加載 OBJ 模型
auto meshResource = resourceManager.loadOBJ(
    "model.obj",
    "materials/",  // MTL 基礎目錄
    LoadMode::Async
);

auto mesh = meshResource->get();
```

### 批量加載

```cpp
// 並行加載多個資源
std::vector<std::shared_ptr<Resource<Image>>> textures;

for (const auto& path : texturePaths) {
    textures.push_back(
        resourceManager.loadImage(path, LoadMode::Async)
    );
}

// 等待所有資源加載完成
resourceManager.waitForAll();

// 處理加載結果
for (auto& texResource : textures) {
    if (texResource->isReady()) {
        auto texture = texResource->get();
        renderer->uploadTexture(texture);
    } else if (texResource->isFailed()) {
        fmt::print("Failed: {}\n", texResource->getError());
    }
}
```

### 緩存管理

```cpp
// 獲取緩存統計
size_t imageCount = resourceManager.getImageCacheSize();
size_t sceneCount = resourceManager.getSceneCacheSize();

// 清除特定緩存
resourceManager.clearImageCache();
resourceManager.clearSceneCache();

// 清除所有緩存
resourceManager.clearAllCaches();
```

### 任務管理

```cpp
// 檢查是否有待處理的加載
if (resourceManager.hasPendingLoads()) {
    size_t count = resourceManager.getActiveLoadCount();
    fmt::print("{} resources loading...\n", count);
}

// 等待所有加載完成
resourceManager.waitForAll();
```

## 與 EngineCore 整合

### 初始化

```cpp
#include "engine_core.hpp"

int main() {
    // 創建引擎核心（自動創建 ResourceManager）
    auto engineCore = std::make_unique<Vapor::EngineCore>();
    engineCore->init();

    // 獲取資源管理器
    auto& resourceManager = engineCore->getResourceManager();

    // 加載資源
    auto scene = resourceManager.loadScene("model.gltf", true);

    // 主循環
    while (running) {
        engineCore->update(deltaTime);
        // ...
    }

    engineCore->shutdown();
}
```

## 實際應用示例

### 示例 1: 異步加載多個紋理

```cpp
void loadMaterial(ResourceManager& rm, const std::string& basePath) {
    // 並行啟動所有紋理加載
    auto albedo = rm.loadImage(basePath + "_albedo.png", LoadMode::Async);
    auto normal = rm.loadImage(basePath + "_normal.png", LoadMode::Async);
    auto metallic = rm.loadImage(basePath + "_metallic.png", LoadMode::Async);
    auto roughness = rm.loadImage(basePath + "_roughness.png", LoadMode::Async);

    // 等待所有紋理加載
    rm.waitForAll();

    // 創建材質
    auto material = std::make_shared<Material>(Material{
        .albedoMap = albedo->get(),
        .normalMap = normal->get(),
        .metallicMap = metallic->get(),
        .roughnessMap = roughness->get()
    });

    return material;
}
```

### 示例 2: 帶進度追蹤的場景加載

```cpp
void loadSceneWithProgress(ResourceManager& rm, const std::string& path) {
    std::atomic<int> progress{0};

    auto sceneResource = rm.loadScene(
        path,
        true,
        LoadMode::Async,
        [&progress](std::shared_ptr<Scene> scene) {
            progress = 100;
            fmt::print("Scene loaded!\n");
        }
    );

    // 顯示加載進度
    while (!sceneResource->isReady() && !sceneResource->isFailed()) {
        fmt::print("\rLoading... {}%", progress.load());
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    if (sceneResource->isFailed()) {
        fmt::print("Error: {}\n", sceneResource->getError());
        return nullptr;
    }

    return sceneResource->get();
}
```

### 示例 3: 資源預加載

```cpp
class ResourcePreloader {
public:
    ResourcePreloader(ResourceManager& rm) : m_rm(rm) {}

    void preloadLevel(const std::string& levelName) {
        // 從配置讀取資源列表
        auto manifest = loadManifest(levelName);

        // 啟動所有異步加載
        for (const auto& texPath : manifest.textures) {
            m_textures.push_back(m_rm.loadImage(texPath, LoadMode::Async));
        }

        for (const auto& modelPath : manifest.models) {
            m_scenes.push_back(m_rm.loadScene(modelPath, true, LoadMode::Async));
        }

        fmt::print("Preloading {} resources...\n",
                   m_textures.size() + m_scenes.size());
    }

    void waitForCompletion() {
        m_rm.waitForAll();
        fmt::print("All resources loaded!\n");
    }

    bool isReady() const {
        return !m_rm.hasPendingLoads();
    }

private:
    ResourceManager& m_rm;
    std::vector<std::shared_ptr<Resource<Image>>> m_textures;
    std::vector<std::shared_ptr<Resource<Scene>>> m_scenes;
};
```

## 架構優勢

### 對比舊系統

| 特性 | 舊 AssetManager | 新 ResourceManager |
|------|-----------------|-------------------|
| API 設計 | 靜態函數 | 實例化對象 |
| 同步/異步 | 分離的類 | 統一接口 |
| 緩存 | 無 | 自動緩存 |
| 狀態追蹤 | 無 | Resource<T> |
| 回調支持 | AsyncAssetLoader only | 統一支持 |
| 類型安全 | 直接返回指針 | Resource<T> 包裝 |
| 錯誤處理 | 異常 | 狀態 + 錯誤消息 |

### 性能優勢

1. **自動緩存**
   - 相同資源只加載一次
   - 減少 I/O 和解碼開銷

2. **真正的並行加載**
   - 多個資源同時加載
   - 充分利用多核心

3. **智能任務調度**
   - 與 enkiTS 深度整合
   - 統一的執行緒池管理

4. **內存優化**
   - 資源共享（`std::shared_ptr`）
   - 可選的緩存清理

## 未來擴展

### 計劃功能

1. **流式加載**
   ```cpp
   // 支持大文件的分塊加載
   auto streamResource = resourceManager.loadStreamedTexture("huge_texture.dds");
   ```

2. **資源熱重載**
   ```cpp
   // 監視文件變化並自動重新加載
   resourceManager.enableHotReload(true);
   ```

3. **LOD 管理**
   ```cpp
   // 根據距離自動切換 LOD
   auto modelResource = resourceManager.loadModel("model.gltf", {
       .enableLOD = true,
       .lodLevels = 3
   });
   ```

4. **內存預算**
   ```cpp
   // 設置資源內存限制
   resourceManager.setMemoryBudget(512 * 1024 * 1024);  // 512MB
   ```

5. **優先級隊列**
   ```cpp
   // 高優先級資源優先加載
   auto critical = resourceManager.loadImage("ui.png", {
       .mode = LoadMode::Async,
       .priority = Priority::High
   });
   ```

## 最佳實踐

### ✅ 推薦做法

1. **始終使用異步加載**（除非明確需要同步）
   ```cpp
   auto resource = rm.loadImage("texture.png", LoadMode::Async);
   ```

2. **利用並行性**
   ```cpp
   // 一次性啟動所有加載
   auto tex1 = rm.loadImage("a.png", LoadMode::Async);
   auto tex2 = rm.loadImage("b.png", LoadMode::Async);
   auto tex3 = rm.loadImage("c.png", LoadMode::Async);
   rm.waitForAll();  // 並行等待
   ```

3. **使用回調避免輪詢**
   ```cpp
   rm.loadScene("scene.gltf", true, LoadMode::Async,
       [](auto scene) {
           // 立即使用加載完成的場景
       }
   );
   ```

4. **定期清理緩存**
   ```cpp
   // 關卡切換時清理
   resourceManager.clearAllCaches();
   ```

### ❌ 避免做法

1. **不要在渲染線程阻塞**
   ```cpp
   // 錯誤：會卡頓
   auto scene = rm.loadScene("huge.gltf", true, LoadMode::Sync)->get();

   // 正確：在加載屏幕異步加載
   auto sceneRes = rm.loadScene("huge.gltf", true, LoadMode::Async);
   while (!sceneRes->isReady()) {
       renderLoadingScreen();
   }
   ```

2. **不要忽略錯誤**
   ```cpp
   auto resource = rm.loadImage("texture.png");
   auto image = resource->get();

   if (resource->isFailed()) {
       fmt::print("Error: {}\n", resource->getError());
       // 使用備用紋理
   }
   ```

## 技術細節

### 線程安全保證

- `Resource<T>`: 完全線程安全
- `ResourceCache<T>`: 內部使用 `std::mutex` 保護
- `ResourceManager`: 所有公共方法線程安全

### 內存管理

- 使用 `std::shared_ptr` 自動管理生命週期
- 緩存保持弱引用（未來計劃）
- 資源在無引用時自動釋放

### 錯誤處理

- 不拋出異常（異步環境不友好）
- 通過狀態和錯誤消息報告問題
- 日誌記錄所有失敗

## 遷移指南

### 從舊 AssetManager 遷移

**Before:**
```cpp
auto image = AssetManager::loadImage("texture.png");
auto scene = AssetManager::loadGLTFOptimized("model.gltf");
```

**After:**
```cpp
ResourceManager& rm = engineCore->getResourceManager();

auto imageRes = rm.loadImage("texture.png");
auto image = imageRes->get();

auto sceneRes = rm.loadScene("model.gltf", true);
auto scene = sceneRes->get();
```

### 從 AsyncAssetLoader 遷移

**Before:**
```cpp
AsyncAssetLoader loader(scheduler);
auto asyncImg = loader.loadImageAsync("texture.png",
    [](auto img) { /* callback */ }
);
```

**After:**
```cpp
ResourceManager rm(scheduler);
auto imgRes = rm.loadImage("texture.png", LoadMode::Async,
    [](auto img) { /* callback */ }
);
```

## 總結

新的資源管理系統提供了：

- 🎯 **統一的 API** - 一個類處理所有資源類型
- 🚀 **真正的異步** - 完全非阻塞的資源加載
- 💾 **自動緩存** - 智能的資源重用
- 🔒 **線程安全** - 可從任何線程安全調用
- 📊 **狀態追蹤** - 清晰的加載狀態和錯誤處理
- 🎨 **現代設計** - 基於模板的類型安全系統

這是一個為未來擴展而設計的系統，將支持流式加載、熱重載、LOD 管理等高級功能。
