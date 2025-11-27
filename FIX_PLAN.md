# Project Vapor 渲染問題修復計畫

## 🔍 問題診斷總結

經過詳細分析，發現以下關鍵事實：

### 1. 架構現狀
專案中存在 **兩套獨立的渲染系統**：

**系統 A（舊版 - 正在使用）:**
- `renderer_legacy.hpp` + `renderer_metal.cpp` + `renderer_vulkan.cpp`
- main.cpp 使用這套系統
- Metal 後端：**完整且功能豐富**
- Vulkan 後端：**已有完整實作但可能有 bug**

**系統 B（新版 - 未使用）:**
- `renderer.hpp` + `renderer.cpp` + `rhi.hpp` + `rhi_vulkan.cpp` + `rhi_metal.cpp`
- main.cpp **完全不知道**這套系統的存在
- 架構優秀但缺少應用層整合

### 2. 為什麼只顯示灰色畫面？

可能原因：
1. **Pipeline 建立失敗** - Shader 編譯或載入問題
2. **Render targets 問題** - 格式或綁定錯誤
3. **Descriptor sets 綁定錯誤** - 資源沒有正確綁定到 shader
4. **ImGui 蓋住內容** - ImGui 的 clear color 覆蓋了場景
5. **Depth test 問題** - Depth buffer 沒有正確設定

### 3. 為什麼 Vulkan 不能跑？

**重要發現：** renderer_vulkan.cpp 其實已經有完整實作！

問題可能出在：
- Shader 路徑或編譯問題（Vulkan 需要 SPIR-V）
- Pipeline 建立參數錯誤
- Validation layers 報錯但被忽略
- Per-mesh buffer 系統與場景載入不匹配

## 🎯 修復策略

### 階段 1：緊急診斷（30 分鐘）

**目標：** 找出為什麼 Metal 後端只顯示灰色

**步驟：**

1. **檢查 Pipeline 建立**
   ```cpp
   // 在 renderer_metal.cpp::createResources() 加入除錯輸出
   drawPipeline = createPipeline("assets/shaders/3d_pbr_normal_mapped.metal", true, false, MSAA_SAMPLE_COUNT);
   if (!drawPipeline) {
       fmt::print("ERROR: Failed to create drawPipeline!\n");
   } else {
       fmt::print("OK: drawPipeline created successfully\n");
   }
   ```

2. **檢查 Shader 檔案是否存在**
   ```bash
   ls -la assets/shaders/*.metal
   ```

3. **檢查 ImGui 的 renderPass 是否正確**
   - ImGui 可能在清除整個畫面

4. **檢查 draw count**
   - 確認是否有東西被繪製
   ```cpp
   // 在 draw() 最後
   fmt::print("Frame {}: Drew {} instances\n", frameNumber, drawCount);
   ```

5. **檢查 clear color**
   - 確認不是因為 clear color 和場景顏色一樣

### 階段 2：Vulkan 後端修復（1-2 小時）

**目標：** 讓 Vulkan 後端能正常顯示畫面

**已知問題：**
1. Vulkan 使用 SPIR-V，需要不同的 shader 檔案
2. Per-mesh buffer 系統需要在 stage() 時建立

**修復步驟：**

#### 2.1 確保 Shader 存在並正確編譯
```bash
# 檢查是否有 SPIR-V shader
ls -la assets/shaders/*.spv

# 如果沒有，需要編譯：
glslangValidator -V shader.vert -o vert.spv
glslangValidator -V shader.frag -o frag.spv
```

#### 2.2 修復 stage() 方法中的 buffer 建立
```cpp
// renderer_vulkan.cpp::stage()
for each mesh:
    // 建立 per-mesh vertex buffer
    mesh->vbos.push_back(createVertexBuffer(mesh->vertices));
    mesh->ebo = createIndexBuffer(mesh->indices);
```

#### 2.3 加入詳細的錯誤檢查
```cpp
// 在每個 Vulkan API 呼叫後
VkResult result = vkCreatePipeline(...);
if (result != VK_SUCCESS) {
    fmt::print("ERROR: vkCreatePipeline failed with error code {}\n", result);
}
```

#### 2.4 檢查 Validation Layers 輸出
```cpp
// 確保 ENABLE_VALIDATION 是 1
#define ENABLE_VALIDATION 1

// 查看控制台輸出的 validation 錯誤
```

### 階段 3：功能驗證（30 分鐘）

**檢查清單：**
- [ ] Metal 後端顯示場景
- [ ] Vulkan 後端顯示場景
- [ ] 光照正確
- [ ] 材質貼圖正確
- [ ] ImGui 可以顯示
- [ ] 攝影機控制正常
- [ ] 沒有 validation errors
- [ ] 效能可接受

## 🛠️ 具體修復程式碼

### 修復 1：加強 renderer_metal.cpp 的除錯輸出

```cpp
// 在 createResources() 最前面加入
fmt::print("=== Creating Metal Resources ===\n");

// 在每個 pipeline 建立後
drawPipeline = createPipeline("assets/shaders/3d_pbr_normal_mapped.metal", true, false, MSAA_SAMPLE_COUNT);
if (!drawPipeline) {
    fmt::print("❌ ERROR: Failed to create drawPipeline!\n");
    throw std::runtime_error("Pipeline creation failed");
} else {
    fmt::print("✓ drawPipeline created\n");
}

// 同樣檢查所有其他 pipelines...

fmt::print("=== Metal Resources Created Successfully ===\n");
```

### 修復 2：加強 renderer_vulkan.cpp 的除錯輸出

```cpp
// 在 init() 最前面加入
fmt::print("=== Initializing Vulkan Renderer ===\n");
fmt::print("Window size: {}x{}\n", windowWidth, windowHeight);

// 在 createPipeline 後
fmt::print("Creating graphics pipelines...\n");
renderPipeline = createPipeline("assets/shaders/vert.spv", "assets/shaders/frag.spv", ...);
if (renderPipeline == VK_NULL_HANDLE) {
    fmt::print("❌ ERROR: Failed to create renderPipeline!\n");
    throw std::runtime_error("Pipeline creation failed");
} else {
    fmt::print("✓ renderPipeline created\n");
}

// 在 draw() 中
fmt::print("Frame {}: Drawing {} instances\n", frameNumber, instances.size());
```

### 修復 3：確保 ImGui 不會覆蓋場景

```cpp
// renderer_metal.cpp::draw()
// ImGui pass 應該設定 LoadOp 為 Load，不是 Clear
auto imguiPassColorRT = imguiPass->colorAttachments()->object(0);
imguiPassColorRT->setLoadAction(MTL::LoadActionLoad);  // ← 關鍵！不要 Clear
imguiPassColorRT->setStoreAction(MTL::StoreActionStore);
imguiPassColorRT->setTexture(surface->texture());
```

### 修復 4：檢查 clear color

```cpp
// 確保 clear color 不是灰色
glm::vec4 clearColor = glm::vec4(0.0f, 0.5f, 1.0f, 1.0f);  // 藍色天空，不是灰色
```

## 🔬 診斷工具

### 工具 1：Pipeline 狀態檢查器
```cpp
void Renderer_Metal::debugPipelineState() {
    fmt::print("\n=== Pipeline State ===\n");
    fmt::print("drawPipeline: {}\n", drawPipeline ? "OK" : "NULL");
    fmt::print("prePassPipeline: {}\n", prePassPipeline ? "OK" : "NULL");
    fmt::print("postProcessPipeline: {}\n", postProcessPipeline ? "OK" : "NULL");
    fmt::print("buildClustersPipeline: {}\n", buildClustersPipeline ? "OK" : "NULL");
    // ... 其他 pipelines
    fmt::print("===================\n\n");
}
```

### 工具 2：繪製統計
```cpp
void Renderer_Metal::debugDrawStats() {
    fmt::print("\n=== Draw Stats ===\n");
    fmt::print("Instances: {}\n", instances.size());
    fmt::print("Draw calls: {}\n", drawCount);
    fmt::print("Visible instances: {}\n", currentInstanceCount);
    fmt::print("Culled instances: {}\n", culledInstanceCount);
    fmt::print("==================\n\n");
}
```

### 工具 3：Material 檢查
```cpp
void Renderer_Metal::debugMaterials() {
    fmt::print("\n=== Materials ===\n");
    for (const auto& [material, meshes] : instanceBatches) {
        fmt::print("Material: {} meshes\n", meshes.size());
        fmt::print("  Albedo: {}\n", material->albedoMap ? material->albedoMap->uri : "none");
        fmt::print("  Normal: {}\n", material->normalMap ? material->normalMap->uri : "none");
    }
    fmt::print("=================\n\n");
}
```

## 📝 檢查清單

### Metal 後端診斷
- [ ] 所有 shader 檔案存在於 `assets/shaders/*.metal`
- [ ] Pipeline 建立成功（無錯誤輸出）
- [ ] Render targets 建立成功
- [ ] Scene 載入成功（instances > 0）
- [ ] Draw calls > 0
- [ ] ImGui 的 LoadAction 是 Load 不是 Clear
- [ ] Clear color 正確（不是灰色）
- [ ] Depth stencil state 正確設定

### Vulkan 後端診斷
- [ ] 所有 shader 檔案存在於 `assets/shaders/*.spv`
- [ ] Validation layers 啟用並無錯誤
- [ ] Instance 建立成功
- [ ] Physical device 選擇成功
- [ ] Logical device 建立成功
- [ ] Swapchain 建立成功
- [ ] Pipeline 建立成功
- [ ] Descriptor sets 建立成功
- [ ] Per-mesh buffers 在 stage() 時建立
- [ ] Command buffer 錄製無錯誤
- [ ] Present 成功

## 🚀 預期結果

修復後應該看到：
- 完整的 Sponza 場景
- 正確的 PBR 材質
- 動態光照
- 移動的點光源
- 可運作的攝影機控制
- ImGui UI 疊加在場景上
- 兩個後端（Metal 和 Vulkan）都能正常運作

## ⚠️ 已知限制

修復後的 Vulkan 版本將**不包含**：
- Ray traced shadows（Metal only）
- Ray traced AO（Metal only）
- Acceleration structures（Metal only）

這是因為 Vulkan 的 ray tracing extensions 太複雜，需要：
- VK_KHR_acceleration_structure
- VK_KHR_ray_tracing_pipeline
- VK_KHR_ray_query
- 額外的 memory 管理
- 更複雜的 descriptor sets

這些功能將在 Vulkan 後端中**跳過**，只保留基本的 rasterization rendering。
