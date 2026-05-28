# RHI 重構階段性計畫

## 📋 當前狀態分析

### RHI 分支現狀（claude/split-renderer-rhi-layer-...）
```
Vapor/include/Vapor/
├── rhi.hpp (13 KB)           ✅ 完整的 RHI 介面
├── rhi_metal.hpp             ✅ Metal RHI 介面定義
├── rhi_vulkan.hpp            ✅ Vulkan RHI 介面定義
├── renderer.hpp (9 KB)       ⚠️  缺少高階 API
├── renderer_metal.hpp        ❌ 舊架構（待移除）
└── renderer_vulkan.hpp       ❌ 舊架構（待移除）

Vapor/src/
├── rhi_metal.cpp (40 KB)     🔨 Metal RHI 實作（部分完成）
├── rhi_vulkan.cpp (63 KB)    🔨 Vulkan RHI 實作（部分完成）
├── renderer.cpp (46 KB)      🔨 Renderer 實作（基礎功能）
├── renderer_metal.cpp        ❌ 舊架構（待移除）
└── renderer_vulkan.cpp       ❌ 舊架構（待移除）
```

**RHI 層已有：**
- ✅ 基礎圖形管線（頂點、片段 shader）
- ✅ 計算管線（compute shader）
- ✅ 加速結構（BLAS/TLAS，ray tracing）
- ✅ 緩衝區、紋理、採樣器管理
- ✅ Render pass / Compute pass
- ✅ 描述符綁定
- ✅ 後端查詢介面（for ImGui）

**Renderer 層已有：**
- ✅ Multi-pass 渲染架構
- ✅ 視錐剔除（frustum culling）
- ✅ Clustered lighting
- ✅ 資源註冊（mesh, material, texture）
- ✅ Drawable 提交與排序

### Main 分支功能（需要移植）

**完整功能列表：**
1. ❌ **Scene/ECS 整合** - `stage(Scene)`, `draw(registry, Scene, Camera)`
2. ❌ **2D/3D 批次渲染** - drawQuad2D/3D, drawLine, drawCircle, etc.
3. ❌ **字型渲染** - loadFont, drawText2D/3D, measureText
4. ❌ **Render-to-Texture** - createRenderTexture, renderToTexture
5. ❌ **後處理效果** - applyBloom, applyToneMapping, applyVignette
6. ❌ **截圖功能** - readPixelsAsync
7. ❌ **ImGui 整合** - 完整的 ImGui 支援
8. ❌ **RmlUI 整合** - initUI()
9. ❌ **DebugDraw 支援** - getDebugDraw()

## 🎯 階段性重構計畫

### ✅ 階段 0：準備工作（當前）
**目標：** 建立計畫並確認方向  
**時間：** 30 分鐘  
**產出：** 本文件

---

### 📝 階段 1：架構統一（2-3 小時）

**目標：** 讓 Renderer 能夠處理 Scene/ECS  

**1.1 擴展 Renderer API**
```cpp
// renderer.hpp 新增方法
class Renderer {
    // Scene/ECS 整合
    void stage(std::shared_ptr<Scene> scene);
    void draw(std::shared_ptr<Scene> scene, Camera& camera);
    void draw(entt::registry& registry, std::shared_ptr<Scene> scene, Camera& camera);
    
    // ImGui 整合
    void setImGuiCallback(std::function<void()> callback);
    
    // 截圖
    void readPixelsAsync(ScreenshotCallback callback);
    
    // UI 支援
    bool initUI();  // RmlUI
    std::shared_ptr<DebugDraw> getDebugDraw();
};
```

**1.2 實作 renderer.cpp**
- 從 main 分支的 renderer_metal.cpp 提取場景遍歷邏輯
- 實作 `collectDrawables()` 從 Scene/ECS 提取 renderables
- 實作 ImGui 初始化（使用 RHI 後端查詢介面）

**產出：**
- ✅ Renderer 可以處理 Scene
- ✅ ImGui 可以運作（透過 RHI backend query）
- ✅ 基本的 draw loop 完整

---

### 🎨 階段 2：批次渲染系統（3-4 小時）

**目標：** 實作 2D/3D 批次渲染  

**2.1 批次渲染架構**
```cpp
// renderer.hpp 新增
class Renderer {
private:
    struct Batch2DRenderer {
        static constexpr uint32_t MaxQuads = 10000;
        static constexpr uint32_t MaxVertices = MaxQuads * 4;
        static constexpr uint32_t MaxIndices = MaxQuads * 6;
        
        BufferHandle vertexBuffer;
        BufferHandle indexBuffer;
        std::vector<Vertex2D> vertices;
        std::vector<uint32_t> indices;
        uint32_t quadCount = 0;
        
        void init(RHI* rhi);
        void shutdown(RHI* rhi);
        void flush(RHI* rhi);
        void reset();
    };
    
    Batch2DRenderer batch2D;
    Batch2DRenderer batch3D;
    
public:
    // 2D API
    void drawQuad2D(const glm::vec2& position, const glm::vec2& size, const glm::vec4& color);
    void drawQuad2D(const glm::mat4& transform, TextureHandle texture, ...);
    void drawLine2D(const glm::vec2& p0, const glm::vec2& p1, ...);
    void drawCircle2D(const glm::vec2& center, float radius, ...);
    void flush2D();
    
    // 3D API (similar)
    void drawQuad3D(...);
    void drawLine3D(...);
    void flush3D();
};
```

**2.2 Batch 渲染管線**
- 創建 batch2D shaders（簡單的 vertex + fragment）
- 創建 batch2D pipeline（alpha blending, no depth test for 2D）
- 創建 batch3D pipeline（with depth test）

**2.3 實作批次邏輯**
- 從 main 分支移植批次積累邏輯
- 實作自動 flush（buffer 滿時）
- 實作紋理切換偵測

**產出：**
- ✅ drawQuad2D/3D 運作
- ✅ drawLine2D/3D 運作
- ✅ drawCircle 等形狀渲染運作
- ✅ 批次統計（draw call count, quad count）

---

### 🔤 階段 3：字型渲染（2-3 小時）

**目標：** 實作字型載入與文字渲染  

**3.1 字型管理器整合**
```cpp
// renderer.hpp 新增
class Renderer {
private:
    std::unique_ptr<FontManager> fontManager;
    
public:
    FontHandle loadFont(const std::string& path, float baseSize);
    void unloadFont(FontHandle handle);
    void drawText2D(FontHandle font, const std::string& text, ...);
    void drawText3D(FontHandle font, const std::string& text, ...);
    glm::vec2 measureText(FontHandle font, const std::string& text, float scale);
    float getFontLineHeight(FontHandle font, float scale);
};
```

**3.2 實作**
- 從 main 分支複製 font_manager.hpp/cpp（如果不存在）
- 整合 FontManager（使用 RHI 創建紋理）
- 使用 batch2D/3D 渲染文字 glyphs

**產出：**
- ✅ loadFont 可載入 TTF/OTF
- ✅ drawText2D 可在螢幕空間繪製文字
- ✅ drawText3D 可在世界空間繪製文字（billboard）

---

### 🖼️ 階段 4：Render-to-Texture（2-3 小時）

**目標：** 實作離屏渲染  

**4.1 RTT 資源管理**
```cpp
// renderer.hpp 新增
struct RenderTextureResource {
    TextureHandle colorTexture;
    TextureHandle depthTexture;
    uint32_t width;
    uint32_t height;
    PixelFormat format;
    bool isHDR;
};

class Renderer {
private:
    std::vector<RenderTextureResource> renderTextures;
    
public:
    RenderTextureHandle createRenderTexture(const RenderTextureDesc& desc);
    void destroyRenderTexture(RenderTextureHandle handle);
    TextureHandle getRenderTextureAsTexture(RenderTextureHandle handle);
    void renderToTexture(RenderTextureHandle target, std::shared_ptr<Scene> scene, Camera& camera, ...);
    glm::uvec2 getRenderTextureSize(RenderTextureHandle handle);
};
```

**4.2 實作**
- 使用 RHI 創建 render target textures
- 修改 beginRenderPass 支援 custom render targets
- 實作 renderToTexture（切換 render target，執行 draw，恢復）

**產出：**
- ✅ createRenderTexture 可創建離屏紋理
- ✅ renderToTexture 可渲染場景到紋理
- ✅ RTT 可作為材質使用（如鏡子、UI 紋理）

---

### ✨ 階段 5：後處理效果（2-3 小時）

**目標：** 實作 bloom, tone mapping, vignette  

**5.1 後處理管線**
```cpp
// renderer.hpp 新增
class Renderer {
private:
    // Bloom resources
    std::vector<TextureHandle> bloomMips;  // Downsampled mips
    ComputePipelineHandle bloomDownsamplePipeline;
    ComputePipelineHandle bloomUpsamplePipeline;
    
    // Tone mapping
    ComputePipelineHandle toneMappingPipeline;
    
    // Vignette
    ComputePipelineHandle vignettePipeline;
    
public:
    void applyBloom(RenderTextureHandle target, float threshold, float strength);
    void applyToneMapping(RenderTextureHandle target, float exposure);
    void applyVignette(RenderTextureHandle target, float strength, float radius);
};
```

**5.2 Compute Shaders**
- 創建 bloom downsample/upsample compute shaders
- 創建 tone mapping compute shader（ACES, Reinhard, etc.）
- 創建 vignette compute shader

**5.3 實作**
- 從 main 分支移植後處理邏輯
- 使用 RHI compute pipeline 執行

**產出：**
- ✅ Bloom 效果運作
- ✅ Tone mapping 運作（HDR -> LDR）
- ✅ Vignette 運作

---

### 🔧 階段 6：完善 RHI 實作（4-5 小時）

**目標：** 完成 RHI_Metal 和 RHI_Vulkan 的完整實作  

**6.1 RHI_Metal 完善**
- 完整實作所有 RHI 介面方法
- 從 main 分支的 renderer_metal.cpp 移植低階邏輯
- 測試所有功能（graphics, compute, ray tracing）

**6.2 RHI_Vulkan 完善**
- 完整實作所有 RHI 介面方法
- 從 main 分支的 renderer_vulkan.cpp 移植低階邏輯
- Stub ray tracing（暫不支援）

**6.3 Shader 移植**
- 複製 main 分支的所有 shaders 到 RHI 分支
- 確保 Metal shaders (.metal) 完整
- 確保 GLSL shaders (.vert/.frag/.comp) 完整
- CMake 自動編譯 SPIR-V

**產出：**
- ✅ RHI_Metal 完全運作
- ✅ RHI_Vulkan 完全運作（除 ray tracing）
- ✅ 所有 shaders 就位

---

### 🧹 階段 7：清理與測試（2-3 小時）

**目標：** 移除舊程式碼，確保一切運作  

**7.1 移除舊架構**
```bash
git rm Vapor/include/Vapor/renderer_metal.hpp
git rm Vapor/include/Vapor/renderer_vulkan.hpp
git rm Vapor/src/renderer_metal.cpp
git rm Vapor/src/renderer_vulkan.cpp
```

**7.2 更新 CMakeLists.txt**
- 移除對舊檔案的引用
- 確保 RHI 檔案都在編譯列表中
- 確保 shader 編譯配置正確

**7.3 測試**
- 測試 Metal 後端所有功能
- 測試 Vulkan 後端所有功能
- 測試批次渲染
- 測試字型渲染
- 測試 RTT
- 測試後處理
- 測試 ray tracing（Metal only）

**7.4 文件更新**
- 更新 README.md
- 創建 RHI_ARCHITECTURE.md 說明新架構
- 更新 IMPLEMENTATION_SUMMARY.md

**產出：**
- ✅ 乾淨的程式碼庫
- ✅ 所有測試通過
- ✅ 文件完整

---

## 📊 時間預估

| 階段 | 描述 | 預估時間 |
|------|------|----------|
| 0 | 準備工作 | 0.5 小時 |
| 1 | 架構統一 | 2-3 小時 |
| 2 | 批次渲染 | 3-4 小時 |
| 3 | 字型渲染 | 2-3 小時 |
| 4 | RTT | 2-3 小時 |
| 5 | 後處理 | 2-3 小時 |
| 6 | RHI 實作 | 4-5 小時 |
| 7 | 清理測試 | 2-3 小時 |
| **總計** | | **18-24 小時** |

## 🎯 當前進度

- [x] 階段 0：準備工作
- [✅] 階段 1：架構統一（完成 90%）
  - [x] 擴展 renderer.hpp API（所有方法簽名）
  - [x] 新增資料結構（BatchRenderer, RenderTextureResource, etc.）
  - [x] 實作基本 stub（所有方法都有框架）
  - [x] BatchRenderer::addQuad() 完整實作
  - [x] createRenderTexture/destroyRenderTexture 完整實作
  - [x] FontManager 整合框架
  - [x] Batch rendering shaders（Batch2D.vert/frag + 2d_batch.metal）
  - [x] BatchRenderer::init() 完整實作（shader 載入 + 管線創建）
  - [x] BatchRenderer::flush() 完整實作（RHI 繪製調用）
  - [x] **collectDrawables() 框架實作（Scene + ECS）**
  - [x] **完整測試計畫（TEST_BATCH_RENDERING.md）**
  - [ ] 實際測試執行與問題修復
- [🔨] 階段 2：批次渲染（進行中）
  - [x] 基礎批次系統完成
  - [ ] 紋理綁定實作
  - [ ] 自動 flush 實作
  - [ ] Shape 繪製（line, circle, etc.）
- [ ] 階段 3：字型渲染
- [ ] 階段 4：RTT
- [ ] 階段 5：後處理
- [✅] 階段 6：RHI 實作（基本完成）
  - [x] RHI_Metal 檢查（1060 行，功能完整）
  - [x] RHI_Vulkan 檢查（1570 行，功能完整）
  - [x] 確認所有 RHI 方法存在
- [ ] 階段 7：清理測試

## 📝 下一步

**當前狀態：** Phase 1 接近完成，測試計畫就緒！
- ✅ Scene/ECS 整合框架完成
- ✅ 測試策略文件完整（5 個測試案例）
- ✅ 調試工具和故障排除指南
- ✅ 測試腳本模板就緒

**立即行動：**
1. 執行 TEST_BATCH_RENDERING.md 中的測試
2. 根據測試結果修復問題
3. 驗證 batch rendering 實際運作

**接下來：**
1. 完善批次渲染（紋理綁定、自動 flush）
2. 實作字型渲染（使用批次系統）
3. 從 main 分支移植更多細節

**進度：** Phase 1: 90%, Overall: ~45%
