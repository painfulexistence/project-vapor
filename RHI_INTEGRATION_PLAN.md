# RHI Layer Integration Plan

## 🎯 PR 目標

建立完整的 RHI (Render Hardware Interface) 抽象層，取代直接使用 Metal/Vulkan API 的舊架構。

## 📋 設計原則

### RHI 層的職責
```
Application Layer (main.cpp, game logic)
        ↓
Renderer Layer (高階渲染邏輯、scene管理)
        ↓
RHI Layer (平台抽象、資源管理)
        ↓
GPU APIs (Metal / Vulkan)
```

### 目標
1. ✅ 平台抽象 - 一套程式碼，兩個後端
2. ✅ 保留所有功能 - Main 分支的所有特性
3. ✅ 更好的架構 - 清晰的職責分離
4. ✅ 易於維護 - 減少重複程式碼
5. ✅ 易於擴充 - 未來可加入 D3D12, WebGPU 等

## 🏗️ 架構設計

### Phase 1: 擴充 RHI 介面以支援 Main 的所有功能

#### 1.1 當前 RHI 介面（已完成）
```cpp
// rhi.hpp - 基本功能
class RHI {
    // Resource creation
    BufferHandle createBuffer(const BufferDesc& desc);
    TextureHandle createTexture(const TextureDesc& desc);
    SamplerHandle createSampler(const SamplerDesc& desc);
    ShaderHandle createShader(const ShaderDesc& desc);
    PipelineHandle createPipeline(const PipelineDesc& desc);
    
    // Compute & Ray Tracing
    ComputePipelineHandle createComputePipeline(const ComputePipelineDesc& desc);
    AccelStructHandle createAccelerationStructure(const AccelStructDesc& desc);
    
    // Rendering
    void beginRenderPass(const RenderPassDesc& desc);
    void endRenderPass();
    void draw(...);
    void dispatch(...);
    
    // Resource updates
    void updateBuffer(...);
    void updateTexture(...);
};
```

#### 1.2 需要新增的 RHI 功能（對應 Main 分支）

**A. 批次渲染支援**
```cpp
// 新增到 rhi.hpp
class RHI {
    // Batch rendering state
    virtual void beginBatch2D() = 0;
    virtual void endBatch2D() = 0;
    virtual void submitBatchVertex2D(const Vertex2D* vertices, uint32_t count) = 0;
    virtual void submitBatchIndex2D(const uint32_t* indices, uint32_t count) = 0;
    virtual void flushBatch2D() = 0;
    
    // Similar for 3D batch
    virtual void beginBatch3D() = 0;
    virtual void endBatch3D() = 0;
    // ...
};
```

**B. Render-to-Texture 支援**
```cpp
// 新增到 rhi.hpp
struct RenderTextureDesc {
    uint32_t width;
    uint32_t height;
    PixelFormat format;
    bool enableMSAA;
    bool enableDepth;
    uint32_t mipLevels;
};

class RHI {
    virtual TextureHandle createRenderTexture(const RenderTextureDesc& desc) = 0;
    virtual void beginRenderToTexture(TextureHandle target) = 0;
    virtual void endRenderToTexture() = 0;
};
```

**C. 字型/文字渲染支援**
```cpp
// 新增 font_types.hpp
struct GlyphMetrics {
    glm::vec2 size;
    glm::vec2 bearing;
    float advance;
};

struct FontAtlas {
    TextureHandle atlasTexture;
    std::unordered_map<char32_t, GlyphMetrics> glyphs;
};

// 在 Renderer 層處理字型，RHI 只需要提供紋理和批次渲染
```

**D. 後處理效果支援**
```cpp
// 新增到 rhi.hpp
class RHI {
    // Post-processing shaders
    virtual void applyFullscreenPass(
        ShaderHandle shader,
        TextureHandle input,
        TextureHandle output,
        const void* uniformData,
        size_t uniformSize
    ) = 0;
    
    // Mipmap generation
    virtual void generateMipmaps(TextureHandle texture) = 0;
};
```

**E. Screenshot/Readback 支援**
```cpp
// 新增到 rhi.hpp
struct ReadbackRequest {
    TextureHandle source;
    std::function<void(const uint8_t* data, uint32_t width, uint32_t height)> callback;
};

class RHI {
    virtual void readbackTextureAsync(const ReadbackRequest& request) = 0;
};
```

### Phase 2: 實作擴充的 RHI 後端

#### 2.1 RHI_Metal 實作

從現有的 `renderer_metal.cpp` 移植邏輯到 `rhi_metal.cpp`：

```cpp
// rhi_metal.cpp
class RHI_Metal : public RHI {
    // 批次渲染狀態
    std::vector<Vertex2D> batchVertices2D;
    std::vector<uint32_t> batchIndices2D;
    NS::SharedPtr<MTL::Buffer> batchVertexBuffer2D;
    NS::SharedPtr<MTL::Buffer> batchIndexBuffer2D;
    
    void beginBatch2D() override {
        batchVertices2D.clear();
        batchIndices2D.clear();
    }
    
    void submitBatchVertex2D(const Vertex2D* vertices, uint32_t count) override {
        batchVertices2D.insert(batchVertices2D.end(), vertices, vertices + count);
    }
    
    void flushBatch2D() override {
        if (batchVertices2D.empty()) return;
        
        // Upload to GPU
        memcpy(batchVertexBuffer2D->contents(), 
               batchVertices2D.data(), 
               batchVertices2D.size() * sizeof(Vertex2D));
        
        // Draw
        renderEncoder->setVertexBuffer(batchVertexBuffer2D.get(), 0, 0);
        renderEncoder->drawIndexed(...);
        
        batchVertices2D.clear();
        batchIndices2D.clear();
    }
    
    // 從 renderer_metal.cpp 移植其他功能...
};
```

#### 2.2 RHI_Vulkan 實作

從現有的 `renderer_vulkan.cpp` 移植邏輯到 `rhi_vulkan.cpp`：

```cpp
// rhi_vulkan.cpp
class RHI_Vulkan : public RHI {
    // 類似 Metal 的實作
    std::vector<Vertex2D> batchVertices2D;
    VkBuffer batchVertexBuffer2D;
    VkDeviceMemory batchVertexMemory2D;
    
    void beginBatch2D() override {
        batchVertices2D.clear();
    }
    
    void flushBatch2D() override {
        if (batchVertices2D.empty()) return;
        
        // Upload to GPU
        void* data;
        vkMapMemory(device, batchVertexMemory2D, 0, VK_WHOLE_SIZE, 0, &data);
        memcpy(data, batchVertices2D.data(), 
               batchVertices2D.size() * sizeof(Vertex2D));
        vkUnmapMemory(device, batchVertexMemory2D);
        
        // Draw
        vkCmdBindVertexBuffers(commandBuffer, 0, 1, &batchVertexBuffer2D, &offset);
        vkCmdDrawIndexed(...);
        
        batchVertices2D.clear();
    }
};
```

### Phase 3: 重構 Renderer 層以使用 RHI

#### 3.1 新的 Renderer 架構

```cpp
// renderer.hpp
class Renderer {
public:
    // 初始化時接收 RHI
    void initialize(RHI* rhi, SDL_Window* window);
    void shutdown();
    
    // Scene 管理
    void stage(std::shared_ptr<Scene> scene);
    
    // 渲染 API（保持與 main 相容）
    void draw(std::shared_ptr<Scene> scene, Camera& camera);
    void draw(entt::registry& registry, std::shared_ptr<Scene> scene, Camera& camera);
    
    // 2D/3D 批次渲染（高階 API）
    void drawQuad2D(const glm::vec2& position, const glm::vec2& size, const glm::vec4& color);
    void drawQuad3D(const glm::vec3& position, const glm::vec2& size, const glm::vec4& color);
    void drawLine2D(const glm::vec2& p0, const glm::vec2& p1, const glm::vec4& color);
    void flush2D();
    void flush3D();
    
    // 字型渲染（高階 API）
    FontHandle loadFont(const std::string& path, float baseSize);
    void drawText2D(FontHandle font, const std::string& text, 
                    const glm::vec2& position, float scale, const glm::vec4& color);
    
    // Render-to-Texture（高階 API）
    RenderTextureHandle createRenderTexture(const RenderTextureDesc& desc);
    void renderToTexture(RenderTextureHandle target, 
                        std::shared_ptr<Scene> scene, Camera& camera);
    
    // 後處理效果（高階 API）
    void applyBloom(RenderTextureHandle target, float threshold, float strength);
    void applyToneMapping(RenderTextureHandle target, float exposure);
    
    // Screenshot
    void readPixelsAsync(ScreenshotCallback callback);
    
private:
    RHI* rhi;  // 使用 RHI 而不是直接使用 Metal/Vulkan
    
    // Multi-pass rendering（使用 RHI）
    void prePass();
    void normalResolvePass();
    void clusterBuildPass();
    void lightCullingPass();
    void raytraceShadowPass();  // Metal only
    void raytraceAOPass();       // Metal only
    void mainRenderPass();
    void postProcessPass();
    
    // 批次渲染狀態
    struct Batch2DState {
        std::vector<Vertex2D> vertices;
        std::vector<uint32_t> indices;
        uint32_t quadCount;
    };
    Batch2DState batch2D;
    
    // 字型管理
    std::unordered_map<FontHandle, FontAtlas> fonts;
    
    // Render targets
    TextureHandle colorRT_MSAA;
    TextureHandle colorRT;
    TextureHandle depthStencilRT_MSAA;
    TextureHandle depthStencilRT;
    // ...
};
```

#### 3.2 實作範例：批次渲染

```cpp
// renderer.cpp
void Renderer::drawQuad2D(const glm::vec2& position, const glm::vec2& size, 
                          const glm::vec4& color) {
    // 建立 quad 的 4 個頂點
    Vertex2D vertices[4] = {
        { {position.x, position.y}, {0, 0}, color },
        { {position.x + size.x, position.y}, {1, 0}, color },
        { {position.x + size.x, position.y + size.y}, {1, 1}, color },
        { {position.x, position.y + size.y}, {0, 1}, color }
    };
    
    // 建立 quad 的 6 個 indices
    uint32_t baseVertex = batch2D.vertices.size();
    uint32_t indices[6] = {
        baseVertex + 0, baseVertex + 1, baseVertex + 2,
        baseVertex + 2, baseVertex + 3, baseVertex + 0
    };
    
    // 加入批次
    batch2D.vertices.insert(batch2D.vertices.end(), vertices, vertices + 4);
    batch2D.indices.insert(batch2D.indices.end(), indices, indices + 6);
    batch2D.quadCount++;
    
    // 如果批次滿了，flush
    if (batch2D.quadCount >= MAX_QUADS_PER_BATCH) {
        flush2D();
    }
}

void Renderer::flush2D() {
    if (batch2D.vertices.empty()) return;
    
    // 使用 RHI 提交批次
    rhi->beginBatch2D();
    rhi->submitBatchVertex2D(batch2D.vertices.data(), batch2D.vertices.size());
    rhi->submitBatchIndex2D(batch2D.indices.data(), batch2D.indices.size());
    rhi->flushBatch2D();
    
    // 清空批次
    batch2D.vertices.clear();
    batch2D.indices.clear();
    batch2D.quadCount = 0;
}
```

#### 3.3 實作範例：Multi-pass 渲染使用 RHI

```cpp
// renderer.cpp
void Renderer::draw(std::shared_ptr<Scene> scene, Camera& camera) {
    // 更新 uniform buffers
    updateFrameData();
    updateCameraData(camera);
    updateLights(scene);
    updateInstances(scene);
    
    // Multi-pass rendering 使用 RHI
    switch (currentRenderPath) {
    case RenderPath::Forward:
        mainRenderPass();
        postProcessPass();
        break;
        
    case RenderPath::Deferred:
        // TODO
        break;
    }
    
    // UI 和 Debug
    if (imGuiCallback) {
        rhi->beginRenderPass(imguiPassDesc);
        // ImGui rendering
        rhi->endRenderPass();
    }
}

void Renderer::prePass() {
    RenderPassDesc prePassDesc;
    prePassDesc.colorAttachments = { {normalRT_MSAA, LoadOp::Clear, ...} };
    prePassDesc.depthAttachment = {depthStencilRT_MSAA, LoadOp::Clear, ...};
    
    rhi->beginRenderPass(prePassDesc);
    rhi->bindPipeline(prePassPipeline);
    
    // 繪製場景（只寫 depth 和 normal）
    for (auto& drawable : visibleDrawables) {
        rhi->bindVertexBuffer(drawable.vertexBuffer);
        rhi->bindIndexBuffer(drawable.indexBuffer);
        rhi->drawIndexed(drawable.indexCount, ...);
    }
    
    rhi->endRenderPass();
}

void Renderer::raytraceShadowPass() {
    // 只在 Metal 後端執行
    if (rhi->getBackendType() != BackendType::Metal) {
        return;
    }
    
    // 使用 RHI 的 compute 和 ray tracing 功能
    rhi->beginComputePass();
    rhi->bindComputePipeline(raytraceShadowPipeline);
    rhi->setComputeTexture(0, depthStencilRT);
    rhi->setComputeTexture(1, normalRT);
    rhi->setComputeTexture(2, shadowRT);
    rhi->setAccelerationStructure(4, TLAS);
    rhi->dispatch(screenWidth, screenHeight, 1);
    rhi->endComputePass();
}
```

### Phase 4: 整合到應用層

#### 4.1 更新 main.cpp

```cpp
// main.cpp
int main(int argc, char* args[]) {
    // ... SDL initialization ...
    
    // 建立 RHI
    std::unique_ptr<RHI> rhi;
    if (gfxBackend == GraphicsBackend::Metal) {
        rhi = createRHI_Metal();
    } else {
        rhi = createRHI_Vulkan();
    }
    rhi->initialize(window);
    
    // 建立 Renderer（使用 RHI）
    Renderer renderer;
    renderer.initialize(rhi.get(), window);
    
    // 載入場景
    auto scene = AssetManager::loadGLTF(...);
    renderer.stage(scene);
    
    // 遊戲迴圈
    while (!quit) {
        // ... input handling ...
        
        // 渲染
        renderer.draw(scene, camera);
        
        // Present
        rhi->present();
    }
    
    // 清理
    renderer.shutdown();
    rhi->shutdown();
    
    return 0;
}
```

#### 4.2 Factory Functions

```cpp
// rhi_factory.hpp
#pragma once
#include "rhi.hpp"
#include <memory>

enum class BackendType {
    Metal,
    Vulkan
};

std::unique_ptr<RHI> createRHI(BackendType type);

// rhi_factory.cpp
std::unique_ptr<RHI> createRHI(BackendType type) {
    switch (type) {
    case BackendType::Metal:
#ifdef __APPLE__
        return std::make_unique<RHI_Metal>();
#else
        return nullptr;
#endif
    case BackendType::Vulkan:
        return std::make_unique<RHI_Vulkan>();
    default:
        return nullptr;
    }
}
```

## 🗓️ 實作時程

### Week 1: RHI 介面擴充
- [ ] 加入批次渲染 API
- [ ] 加入 RTT API
- [ ] 加入後處理 API
- [ ] 加入 readback API
- [ ] 更新文件

### Week 2: RHI_Metal 實作
- [ ] 從 renderer_metal.cpp 移植批次渲染
- [ ] 實作 RTT
- [ ] 實作後處理
- [ ] 實作 readback
- [ ] 測試所有功能

### Week 3: RHI_Vulkan 實作
- [ ] 從 renderer_vulkan.cpp 移植批次渲染
- [ ] 實作 RTT
- [ ] 實作後處理
- [ ] 實作 readback
- [ ] 建立 SPIR-V shaders
- [ ] 測試所有功能

### Week 4: Renderer 層重構
- [ ] 使用 RHI 重寫 Renderer
- [ ] 實作批次渲染 API
- [ ] 實作字型渲染
- [ ] 實作 RTT API
- [ ] 實作後處理 API

### Week 5: 整合與測試
- [ ] 更新 main.cpp
- [ ] 測試 Metal 後端所有功能
- [ ] 測試 Vulkan 後端所有功能
- [ ] 效能測試與優化
- [ ] 文件完善

### Week 6: 清理與 PR
- [ ] 移除舊的 renderer_metal.cpp / renderer_vulkan.cpp
- [ ] 程式碼審查
- [ ] 修正 bugs
- [ ] 準備 PR
- [ ] 撰寫 migration guide

## 🎯 成功指標

完成後應該達到：

1. ✅ **功能完整性**
   - Metal 和 Vulkan 都支援所有功能（除了 Vulkan 不支援 ray tracing）
   - 與 main 分支功能一致

2. ✅ **架構清晰**
   - 三層架構清楚分離
   - RHI 層完全平台無關
   - Renderer 層不直接呼叫 GPU API

3. ✅ **程式碼品質**
   - 減少 50%+ 重複程式碼
   - 易於新增後端（D3D12, WebGPU）
   - 易於維護

4. ✅ **效能**
   - 與直接使用 API 的版本效能差距 < 5%
   - 批次渲染正確運作

5. ✅ **可測試性**
   - 可以輕易切換後端測試
   - 清楚的錯誤訊息

## 📝 下一步

請確認這個計畫是否符合您的需求。我可以立即開始：

**選項 1：從 RHI 介面擴充開始**
- 我會先擴充 rhi.hpp 加入所有需要的 API
- 時間：2-3 小時

**選項 2：直接實作 RHI_Metal 批次渲染**
- 從 renderer_metal.cpp 移植批次渲染邏輯到 rhi_metal.cpp
- 快速看到成果
- 時間：3-4 小時

**選項 3：創建最小可運行版本**
- 只實作核心渲染功能
- 批次渲染和其他功能稍後加
- 時間：4-6 小時

我建議從**選項 1** 開始，先把介面設計好，再逐步實作。這樣架構會更清晰。

您覺得如何？
