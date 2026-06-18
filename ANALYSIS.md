# Project Vapor Renderer Architecture Analysis

## 問題概述

目前專案中存在**兩套完全不同的渲染系統**同時共存，但彼此之間沒有整合，導致新的 RHI 架構無法運作。

## 當前架構狀況

### 1. 舊渲染系統（Legacy Renderer - 正在使用）

**檔案：**
- `renderer_legacy.hpp` - 抽象基類定義
- `renderer_metal.cpp/hpp` - Metal 後端實作
- `renderer_vulkan.cpp/hpp` - Vulkan 後端實作

**介面：**
```cpp
class Renderer {
    virtual void init(SDL_Window* window) = 0;
    virtual void deinit() = 0;
    virtual void stage(std::shared_ptr<Scene> scene) = 0;
    virtual void draw(std::shared_ptr<Scene> scene, Camera& camera) = 0;
    virtual void setRenderPath(RenderPath path) = 0;
    virtual RenderPath getRenderPath() const = 0;
};
```

**使用方式（main.cpp）：**
```cpp
auto renderer = createRenderer(gfxBackend);  // 建立 Renderer_Metal 或 Renderer_Vulkan
renderer->init(window);
renderer->stage(scene);  // 載入場景資源
// 遊戲迴圈：
renderer->draw(scene, camera);  // 每幀渲染
```

**特性：**
- ✅ **Metal 後端完整實作** - 包含所有進階功能
  - Multi-pass rendering (pre-pass, compute passes, main pass, post-process)
  - Clustered lighting (16x16x24 grid)
  - Ray traced shadows (Metal acceleration structures)
  - Ray traced AO
  - MSAA 4x
  - HDR rendering with tone mapping
- ❌ **Vulkan 後端僅有骨架** - 只有基本結構，沒有實作

### 2. 新渲染系統（New Renderer with RHI - 未使用）

**檔案：**
- `rhi.hpp` - RHI 抽象層介面
- `rhi_vulkan.cpp/hpp` - Vulkan RHI 實作
- `rhi_metal.cpp/hpp` - Metal RHI 實作
- `renderer.hpp/cpp` - 高階渲染器（使用 RHI）
- `render_data.hpp` - 渲染資料結構

**介面：**
```cpp
class Renderer {
    void initialize(RHI* rhi);
    void shutdown();
    MeshId registerMesh(...);
    MaterialId registerMaterial(...);
    void beginFrame(const CameraRenderData& camera);
    void submitDrawable(const Drawable& drawable);
    void submitDirectionalLight(const DirectionalLightData& light);
    void submitPointLight(const PointLightData& light);
    void render();
    void endFrame();
    void setRenderPath(RenderPath path);
};
```

**設計理念：**
```cpp
// 應該這樣使用（但 main.cpp 沒有這樣做）
RHI* rhi = createRHI_Vulkan();  // 或 createRHI_Metal()
rhi->initialize(window);

Renderer renderer;
renderer.initialize(rhi);

// 註冊資源
MeshId meshId = renderer.registerMesh(vertices, indices);
MaterialId matId = renderer.registerMaterial(materialData);

// 每幀渲染
renderer.beginFrame(camera);
renderer.submitDrawable({meshId, matId, transform});
renderer.submitPointLight(light);
renderer.render();  // 執行 multi-pass rendering
renderer.endFrame();
```

**特性：**
- ✅ **完整的 RHI 抽象層**
  - Vulkan 和 Metal 都有完整實作
  - 支援 compute pipelines
  - 支援 acceleration structures (Metal)
  - 統一的資源管理
- ✅ **高階 Renderer 類別擴充**
  - Multi-pass rendering 方法都已定義
  - 所有 render targets、pipelines、buffers 都已加入
- ❌ **完全沒有與應用層整合** - main.cpp 不知道它的存在
- ❌ **缺少關鍵實作**
  - 沒有實際的繪製邏輯（executeDrawCalls 是空的）
  - 沒有 pipeline 建立邏輯（shader 載入）
  - 沒有資源註冊與場景整合

## 功能對比：舊 renderer_metal.cpp vs 新 Renderer

### ✅ 舊 renderer_metal.cpp 有但新 Renderer 缺少的功能

#### 1. **場景資源載入 (stage 方法)**
```cpp
// 舊：renderer_metal.cpp:194-271
void Renderer_Metal::stage(std::shared_ptr<Scene> scene) {
    // 建立光源 buffers
    directionalLightBuffer = device->newBuffer(scene->directionalLights.size() * sizeof(DirectionalLight), ...);
    pointLightBuffer = device->newBuffer(scene->pointLights.size() * sizeof(PointLight), ...);

    // 載入材質貼圖
    for (auto& img : scene->images) {
        img->texture = createTexture(img);
    }

    // 建立材質
    materialDataBuffer = device->newBuffer(scene->materials.size() * sizeof(MaterialData), ...);

    // 建立場景 vertex/index buffers
    scene->vertexBuffer = createVertexBuffer(scene->vertices);
    scene->indexBuffer = createIndexBuffer(scene->indices);

    // 為每個 mesh 建立 BLAS (acceleration structure)
    for each mesh:
        auto accelStruct = buildBLAS(mesh);
        BLASs.push_back(accelStruct);
}
```

**新 Renderer 狀態：** ❌ 完全缺少
- `registerMesh()` 和 `registerMaterial()` 存在，但需要逐個呼叫
- 沒有批次載入整個場景的方法
- 沒有建立 acceleration structures 的實作（buildAccelerationStructures() 是空的）

#### 2. **Pipeline 建立**
```cpp
// 舊：renderer_metal.cpp:73-81
drawPipeline = createPipeline("assets/shaders/3d_pbr_normal_mapped.metal", true, false, 4);
prePassPipeline = createPipeline("assets/shaders/3d_depth_only.metal", true, false, 4);
postProcessPipeline = createPipeline("assets/shaders/3d_post_process.metal", false, true, 1);
buildClustersPipeline = createComputePipeline("assets/shaders/3d_cluster_build.metal");
cullLightsPipeline = createComputePipeline("assets/shaders/3d_light_cull.metal");
tileCullingPipeline = createComputePipeline("assets/shaders/3d_tile_light_cull.metal");
normalResolvePipeline = createComputePipeline("assets/shaders/3d_normal_resolve.metal");
raytraceShadowPipeline = createComputePipeline("assets/shaders/3d_raytrace_shadow.metal");
raytraceAOPipeline = createComputePipeline("assets/shaders/3d_ssao.metal");
```

**新 Renderer 狀態：** ❌ 缺少 shader 載入和 pipeline 建立
- `createRenderPipeline()` 在 initialize() 中被註解掉
- 沒有 shader 檔案載入機制
- 所有 pipeline handles 都是 invalid

#### 3. **完整的繪製邏輯 (draw 方法)**
```cpp
// 舊：renderer_metal.cpp:273-700+ (超過 400 行)
void Renderer_Metal::draw(std::shared_ptr<Scene> scene, Camera& camera) {
    // 1. 準備 frame data (時間、frame number)
    // 2. 更新 camera uniform buffer
    // 3. 更新光源 buffers
    // 4. 更新材質 buffer
    // 5. 收集所有 instances (遍歷場景樹)
    // 6. 更新 instance buffer
    // 7. 建立/更新 TLAS (top-level acceleration structure)

    // 8. Multi-pass rendering:
    //    a. Build TLAS
    //    b. Pre-pass (depth + normals)
    //    c. Normal resolve (compute)
    //    d. Tile culling (compute)
    //    e. Raytrace shadows (compute + ray tracing)
    //    f. Raytrace AO (compute + ray tracing)
    //    g. Main render pass (PBR + clustered lighting)
    //    h. Post-process (tone mapping)
    //    i. ImGui pass

    // 9. Present
}
```

**新 Renderer 狀態：** ⚠️ 部分實作
- ✅ Multi-pass 方法都已定義（prePass, normalResolvePass, etc.）
- ❌ `executeDrawCalls()` 是空的 - 沒有實際繪製
- ❌ 沒有場景遍歷邏輯
- ❌ 沒有 instance batching
- ❌ 沒有 TLAS 建立/更新
- ❌ 沒有 ImGui 整合

#### 4. **Instance Batching 和材質分組**
```cpp
// 舊：renderer_metal.cpp:351-392
instances.clear();
accelInstances.clear();
instanceBatches.clear();  // std::unordered_map<Material*, std::vector<Mesh*>>

// 遍歷場景樹收集所有 instances
for each node in scene:
    if node->meshGroup:
        for each mesh:
            instances.push_back(InstanceData{...});
            accelInstances.push_back(MTL::AccelerationStructureInstanceDescriptor{...});
            instanceBatches[mesh->material].push_back(mesh);

// 繪製時按材質分組以減少狀態切換
for (const auto& [material, meshes] : instanceBatches) {
    bindMaterial(material);
    for (const auto& mesh : meshes) {
        if (camera.isVisible(mesh->boundingSphere)) {
            drawIndexedPrimitives(...);
        }
    }
}
```

**新 Renderer 狀態：** ❌ 缺少
- 有 `frameDrawables` 和 `visibleDrawables`，但沒有填入資料
- `performCulling()` 有實作但沒有輸入
- `sortDrawables()` 有實作但沒有繪製

#### 5. **ImGui 整合**
```cpp
// 舊：renderer_metal.cpp:49-50, 63-64
ImGui_ImplSDL3_InitForMetal(window);
ImGui_ImplMetal_Init(device);

// 在 draw() 中：
ImGui_ImplMetal_NewFrame(imguiPass.get());
ImGui_ImplSDL3_NewFrame();
ImGui::NewFrame();
// ... ImGui UI code ...
ImGui::Render();
ImGui_ImplMetal_RenderDrawData(ImGui::GetDrawData(), commandBuffer, renderEncoder);
```

**新 Renderer 狀態：** ❌ 完全缺少

#### 6. **Depth Stencil State**
```cpp
// 舊：renderer_metal.cpp:186-191
MTL::DepthStencilDescriptor* depthStencilDesc = MTL::DepthStencilDescriptor::alloc()->init();
depthStencilDesc->setDepthCompareFunction(MTL::CompareFunction::CompareFunctionLessEqual);
depthStencilDesc->setDepthWriteEnabled(true);
depthStencilState = device->newDepthStencilState(depthStencilDesc);

// 使用時：
renderEncoder->setDepthStencilState(depthStencilState.get());
```

**新 Renderer 狀態：** ❌ RHI 層沒有 depth stencil state 概念

### ✅ 新 Renderer 有但舊版缺少的功能

#### 1. **平台抽象**
- ✅ 完整的 RHI 層，可以輕易切換 Vulkan/Metal
- ✅ 統一的資源管理介面
- ✅ 更清晰的職責分離

#### 2. **更現代的架構**
- ✅ 資源 ID 系統 (MeshId, MaterialId, TextureId)
- ✅ Drawable submission 模型
- ✅ Frame-based rendering flow

## 問題診斷

### 為什麼只顯示灰色畫面和 ImGui？

1. **main.cpp 使用的是舊 renderer_metal.cpp**
   - 舊的 Metal renderer 實作完整，應該可以正常工作
   - 但可能有某些初始化問題

2. **新的 Renderer + RHI 系統完全沒有被使用**
   - main.cpp 完全不知道新系統的存在
   - 新系統的 `executeDrawCalls()` 是空的

3. **可能的問題：**
   - Pipeline 建立失敗（shader 載入問題）
   - Render targets 建立失敗
   - Command buffer 執行問題
   - ImGui 蓋掉了所有內容

### 為什麼 Vulkan 後端無法運作？

**重大更新：** renderer_vulkan.cpp 其實已經有相當完整的實作！

經檢查發現 `renderer_vulkan.cpp` 已經實作了：
- ✅ 完整的 Vulkan 初始化 (instance, device, swapchain)
- ✅ Multi-pass rendering (pre-pass, main pass, post-process)
- ✅ Tile-based light culling (compute shader)
- ✅ 場景遍歷和 instance 收集
- ✅ Material 綁定和 descriptor sets
- ✅ Render targets (depth, color, etc.)
- ✅ Pipeline 建立
- ⚠️ **問題在於：**
  - 使用舊的 per-mesh buffer 系統 (`mesh->vbos[0]`, `mesh->ebo`)
  - 與新的 RHI 架構不相容
  - 可能有 shader 或 pipeline 建立問題

## 修復策略

### 選項 A：修復並使用新的 RHI 架構（推薦）

**優點：**
- 更好的架構，易於維護
- Vulkan 和 Metal 平等支援
- 未來擴充性好

**缺點：**
- 需要大量工作
- 需要重構 main.cpp

**工作項目：**
1. 實作 shader 載入系統
2. 實作 pipeline 建立邏輯
3. 實作場景載入 (類似舊的 stage())
4. 實作完整的繪製邏輯 (類似舊的 draw())
5. 實作 TLAS/BLAS 建立
6. 整合 ImGui
7. 更新 main.cpp 使用新 API
8. 新增 depth stencil state 到 RHI
9. 完整實作所有 multi-pass 方法的內部邏輯

### 選項 B：在舊系統上修復 Vulkan 後端

**優點：**
- 不需要改動 main.cpp
- 可以參考 renderer_metal.cpp

**缺點：**
- 維護兩套系統
- 新的 RHI 工作白費
- 長期技術債

**工作項目：**
1. 將 renderer_metal.cpp 的邏輯移植到 renderer_vulkan.cpp
2. 替換所有 Metal API 為 Vulkan API
3. 實作 Vulkan 版本的 pipeline、buffer、texture 建立
4. 實作 Vulkan command buffer 錄製
5. 跳過 ray tracing 相關功能（Vulkan RT extension 太複雜）

### 選項 C：混合方案

**階段 1：短期修復（讓 Vulkan 能跑）**
- 實作最基本的 Vulkan renderer（單 pass, forward rendering）
- 讓畫面顯示出來

**階段 2：中期遷移（整合到新架構）**
- 逐步將功能遷移到新的 RHI + Renderer 系統
- 先 Metal，後 Vulkan

## 建議的修復順序（選項 C - 混合方案）

### Phase 1: 緊急修復 - 讓畫面顯示出來

1. ✅ **檢查並修復 renderer_metal.cpp 的問題**
   - 確認 pipeline 建立成功
   - 確認 render targets 正確
   - 檢查 command buffer 提交

2. ✅ **實作基本的 renderer_vulkan.cpp**
   - 參考 renderer_metal.cpp 結構
   - 僅實作 forward rendering（無 compute, 無 ray tracing）
   - 使用新的 RHI_Vulkan 底層

### Phase 2: 架構遷移 - 整合新系統

3. **實作新 Renderer 的缺失功能**
   - Shader 載入
   - Pipeline 建立
   - 場景載入
   - 繪製邏輯
   - ImGui 整合

4. **更新 main.cpp 使用新 API**

5. **棄用舊的 renderer_legacy.hpp 系統**

## 結論

目前專案的主要問題是**有兩套渲染系統但沒有整合**。建議採用**混合方案**：
1. 先快速修復讓畫面顯示
2. 再逐步遷移到新架構

這樣可以：
- ✅ 快速看到成果
- ✅ 保留新架構的投資
- ✅ 漸進式重構，降低風險
