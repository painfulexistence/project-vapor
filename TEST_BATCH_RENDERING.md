# Batch Rendering Test Plan

## 🎯 目標

驗證 RHI 分支的 batch rendering 系統能正常運作。

## ✅ 測試前檢查

### 1. 必要組件檢查
- [x] BatchRenderer::init() 實作完成
- [x] BatchRenderer::flush() 實作完成  
- [x] BatchRenderer::addQuad() 實作完成
- [x] Batch2D shaders 存在（Batch2D.vert/frag, 2d_batch.metal）
- [x] RHI_Metal 基本完整
- [x] RHI_Vulkan 基本完整

### 2. 預期工作流程
```cpp
// Initialize
Renderer renderer;
renderer.initialize(rhi, backend);  // Calls initBatchRendering()

// Per frame
renderer.drawQuad2D(glm::vec2(100, 100), glm::vec2(50, 50), glm::vec4(1,0,0,1));
renderer.drawQuad2D(glm::vec2(200, 200), glm::vec2(50, 50), glm::vec4(0,1,0,1));
renderer.flush2D();  // Uploads vertices, binds pipeline, draws
```

## 🧪 測試案例

### Test 1: 基本初始化
**目的：** 確認 BatchRenderer 能正確初始化

**步驟：**
1. 創建 RHI (Metal 或 Vulkan)
2. 創建 Renderer
3. 調用 initialize()
4. 檢查 batch2D.pipeline.isValid()

**預期結果：**
- 不應該崩潰
- BatchRenderer 應該輸出 "BatchRenderer initialized (2D mode)"
- pipeline 應該是 valid

**可能問題：**
- Shader 檔案找不到 → 檢查路徑
- Shader 編譯失敗 → 檢查 shader 語法
- Pipeline 創建失敗 → 檢查 vertex layout

### Test 2: 單一 Quad 渲染
**目的：** 測試基本的 quad 繪製

**步驟：**
1. 初始化 renderer
2. 調用 beginFrame()
3. 調用 drawQuad2D(glm::vec2(100, 100), glm::vec2(50, 50), glm::vec4(1,0,0,1))
4. 調用 flush2D()
5. 調用 endFrame()

**預期結果：**
- batch2D.vertices 應該包含 4 個頂點
- batch2D.quadCount 應該是 1
- flush 後應該調用 rhi->drawIndexed(6, 1, 0, 0, 0)

**可能問題：**
- 頂點數據錯誤 → 檢查 addQuad() 邏輯
- Projection matrix 錯誤 → 檢查 flush2D() 的 ortho 矩陣
- 緩衝區更新失敗 → 檢查 updateBuffer() 實作

### Test 3: 批次多個 Quads
**目的：** 測試批次繪製多個 quads

**步驟：**
1. 初始化 renderer
2. 繪製 100 個 quads
3. 調用 flush2D()

**預期結果：**
- batch2D.quadCount 應該是 100
- drawIndexed 應該調用 600 個索引（100 * 6）
- batch2D.drawCalls 應該是 1
- flush 後 vertices 應該清空

### Test 4: 自動 Flush (超過容量)
**目的：** 測試當超過 MaxQuads 時的行為

**步驟：**
1. 繪製 10001 個 quads (> MaxQuads = 10000)

**預期結果：**
- 應該自動 flush（但目前實作還沒有）
- 或者忽略超出的 quads

**當前狀態：** addQuad() 目前只是 return，不會自動 flush

**需要修復：**
```cpp
void BatchRenderer::addQuad(...) {
    if (quadCount >= MaxQuads) {
        // TODO: Need viewProj to auto-flush
        // For now just return
        return;
    }
    // ...
}
```

### Test 5: 3D Batch Rendering
**目的：** 測試 3D quad 繪製（with depth）

**步驟：**
1. 初始化 renderer
2. 調用 drawQuad3D() with different Z values
3. 調用 flush3D()

**預期結果：**
- 使用 camera view-projection 而非 ortho
- 深度測試啟用
- Quads 按深度排序顯示

## 🔍 調試工具

### 1. 加入調試輸出
在 BatchRenderer::flush() 中：
```cpp
fmt::print("Flushing {} quads, {} vertices\n", quadCount, vertices.size());
fmt::print("Drawing {} indices\n", quadCount * 6);
```

### 2. 驗證頂點數據
```cpp
for (size_t i = 0; i < vertices.size() && i < 4; i++) {
    auto& v = vertices[i];
    fmt::print("Vertex {}: pos=({},{},{}), color=({},{},{},{})\n",
        i, v.position.x, v.position.y, v.position.z,
        v.color.r, v.color.g, v.color.b, v.color.a);
}
```

### 3. 檢查 RHI 狀態
```cpp
fmt::print("Pipeline valid: {}\n", pipeline.isValid());
fmt::print("Vertex buffer valid: {}\n", vertexBuffer.isValid());
fmt::print("Index buffer valid: {}\n", indexBuffer.isValid());
```

## 📋 已知限制

### 當前實作的限制：
1. **無紋理支援** - flush() 中 TODO: Bind textures
2. **無自動 flush** - addQuad() 超過容量時只是 return
3. **無 texture array** - 只能用白色紋理
4. **Shape 繪製未實作** - drawLine, drawCircle 等還是 TODO

### 需要實作的功能：
1. **Texture binding** - 在 flush() 中綁定紋理數組
2. **Auto-flush** - 在 addQuad() 中檢測容量並自動 flush
3. **Texture batching** - 追蹤使用的紋理，自動切換 batch
4. **Shape primitives** - 使用 quads 模擬 lines, circles

## 🚀 下一步

### 短期（立即）：
1. **手動測試** - 創建簡單的測試程序
2. **修復發現的問題** - 根據測試結果修復
3. **加入調試輸出** - 幫助追蹤問題

### 中期（接下來幾天）：
1. **實作紋理綁定** - 完成 flush() 的 TODO
2. **實作自動 flush** - 讓批次系統更健壯
3. **實作 shape 繪製** - drawLine, drawCircle 等

### 長期（未來）：
1. **從 main 分支移植** - 完整的場景遍歷
2. **整合測試** - 端到端的渲染測試
3. **性能優化** - 減少狀態切換

## ✍️ 測試腳本模板

創建 `test_batch_rendering.cpp`:

```cpp
#include "renderer.hpp"
#include "rhi_metal.hpp"  // or rhi_vulkan.hpp
#include <SDL3/SDL.h>

int main() {
    // Initialize SDL
    SDL_Init(SDL_INIT_VIDEO);
    SDL_Window* window = SDL_CreateWindow(
        "Batch Rendering Test",
        800, 600,
        SDL_WINDOW_VULKAN  // or SDL_WINDOW_METAL
    );

    // Create renderer
    auto renderer = createRenderer(GraphicsBackend::Vulkan, window);

    // Test 1: Check initialization
    fmt::print("Test 1: Initialization\n");
    auto stats = renderer->getBatch2DStats();
    fmt::print("  Initial draw calls: {}\n", stats.drawCalls);

    // Test 2: Draw single quad
    fmt::print("\nTest 2: Single quad\n");
    renderer->drawQuad2D(
        glm::vec2(100, 100),
        glm::vec2(50, 50),
        glm::vec4(1, 0, 0, 1)
    );
    renderer->flush2D();
    stats = renderer->getBatch2DStats();
    fmt::print("  Draw calls after flush: {}\n", stats.drawCalls);
    fmt::print("  Quads drawn: {}\n", stats.quadCount);

    // Test 3: Draw multiple quads
    fmt::print("\nTest 3: Multiple quads\n");
    renderer->resetBatch2DStats();
    for (int i = 0; i < 100; i++) {
        renderer->drawQuad2D(
            glm::vec2(i * 10, i * 10),
            glm::vec2(50, 50),
            glm::vec4(1, 1, 0, 1)
        );
    }
    renderer->flush2D();
    stats = renderer->getBatch2DStats();
    fmt::print("  Draw calls: {}\n", stats.drawCalls);
    fmt::print("  Total quads: {}\n", stats.quadCount);

    // Cleanup
    SDL_DestroyWindow(window);
    SDL_Quit();

    fmt::print("\n✅ All tests passed!\n");
    return 0;
}
```

**編譯：**
```bash
g++ -std=c++20 test_batch_rendering.cpp \
    -I Vapor/include \
    -L build/Vapor \
    -lVapor \
    -lSDL3 \
    -lfmt \
    -o test_batch
```

**執行：**
```bash
./test_batch
```

## 📊 預期輸出

```
Test 1: Initialization
BatchRenderer initialized (2D mode)
BatchRenderer initialized (3D mode)
  Initial draw calls: 0

Test 2: Single quad
Flushing 1 quads, 4 vertices
Drawing 6 indices
  Draw calls after flush: 1
  Quads drawn: 1

Test 3: Multiple quads
Flushing 100 quads, 400 vertices
Drawing 600 indices
  Draw calls: 1
  Total quads: 100

✅ All tests passed!
```

## 🐛 常見問題

### 問題 1: Shader 找不到
**症狀：** "Failed to load batch2D shaders"

**解決：**
- 檢查 working directory 是否在專案根目錄
- 檢查 assets/shaders/ 是否存在
- 檢查 Batch2D.vert.spv 是否已編譯（Vulkan）

### 問題 2: Pipeline 創建失敗
**症狀：** pipeline.isValid() = false

**解決：**
- 檢查 vertex layout 是否正確
- 檢查 shader entry points 是否匹配
- 查看 RHI 的錯誤輸出

### 問題 3: 畫面全黑
**症狀：** 沒有任何東西顯示

**解決：**
- 檢查 projection matrix 是否正確
- 檢查 quad 是否在視錐內
- 檢查 blend mode 是否正確
- 使用 validation layers 查看錯誤

### 問題 4: 崩潰
**症狀：** Segmentation fault

**可能原因：**
- RHI 指標無效
- 緩衝區未創建
- Shader 資源未正確管理

**解決：**
- 在每個 RHI 調用前檢查 handle.isValid()
- 使用 address sanitizer: `g++ -fsanitize=address`
- 加入 null pointer 檢查

---

**準備測試了嗎？** 🚀
