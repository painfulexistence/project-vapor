# Mesh Particles 擴充設計

## 概述

本文件討論如何在現有粒子系統中加入 Mesh Particles 支援。目前系統僅支援 Billboard 渲染，需要擴展以支援 3D Mesh 實例化渲染。

---

## 現有架構回顧

```
┌─────────────────────────────────────────────────────────────┐
│                    GPUParticle Buffer                        │
│  position, velocity, color, life, emitterID                  │
└─────────────────────────────────────────────────────────────┘
                           │
                           ▼
                ┌─────────────────────┐
                │   Billboard Pass    │
                │   (6 verts/quad)    │
                │   Instanced draw    │
                └─────────────────────┘
```

---

## 方案 A：共用 Buffer + 不同 Pipeline

### 設計概念

所有粒子類型共用同一個 `GPUParticle` buffer，透過 emitter 參數決定使用哪個渲染 pipeline。

```
┌─────────────────────────────────────────────────────────────┐
│                   GPUParticle Buffer (擴展)                   │
│  position, velocity, color, life, emitterID                  │
│  + rotation (quat)                                           │
│  + scale                                                     │
│  + meshIndex (optional)                                      │
└─────────────────────────────────────────────────────────────┘
                           │
           ┌───────────────┼───────────────┐
           ▼               ▼               ▼
    ┌─────────────┐ ┌─────────────┐ ┌─────────────┐
    │  Billboard  │ │    Mesh     │ │   Ribbon    │
    │   Pipeline  │ │  Pipeline   │ │  Pipeline   │
    └─────────────┘ └─────────────┘ └─────────────┘
```

### 數據結構修改

#### GPUParticle 擴展

```cpp
// Vapor/include/Vapor/graphics.hpp

struct alignas(16) GPUParticle {
    // 現有欄位
    glm::vec3 position = glm::vec3(0.0f);
    float _pad1 = 0.0f;
    glm::vec3 velocity = glm::vec3(0.0f);
    float _pad2 = 0.0f;
    glm::vec3 force = glm::vec3(0.0f);
    float _pad3 = 0.0f;
    glm::vec4 color = glm::vec4(1.0f);
    float life = 1.0f;
    float age = 0.0f;
    float maxLife = 1.0f;
    Uint32 emitterID = 0;

    // 新增：Mesh Particles 需要
    glm::quat rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);  // 16 bytes
    float scale = 1.0f;
    Uint32 meshIndex = 0;      // 使用哪個預載 mesh
    float _pad4[2] = {0.0f};
};
```

#### EmitterParams 擴展

```cpp
enum class ParticleRenderMode : Uint32 {
    Billboard = 0,
    Mesh = 1,
    Ribbon = 2,  // 未來
};

struct alignas(16) ParticleEmitterGPUData {
    // 現有欄位...

    // 新增
    Uint32 renderMode = 0;      // ParticleRenderMode
    Uint32 meshIndex = 0;       // 預載 mesh 的索引
    Uint32 alignToVelocity = 0; // Mesh 是否對齊速度方向
    float _padRender = 0.0f;
};
```

### 渲染流程

```cpp
void ParticlePass::execute() {
    // ... compute passes ...

    // 收集每種 renderMode 的 emitter 範圍
    struct RenderBatch {
        Uint32 startIndex;
        Uint32 count;
        Uint32 meshIndex;
    };
    std::vector<RenderBatch> billboardBatches;
    std::vector<RenderBatch> meshBatches;

    for (auto& emitter : emitters) {
        RenderBatch batch = { emitter.startIndex, emitter.maxParticles, emitter.meshIndex };
        if (emitter.renderMode == Billboard) {
            billboardBatches.push_back(batch);
        } else if (emitter.renderMode == Mesh) {
            meshBatches.push_back(batch);
        }
    }

    // Pass 1: Billboard particles
    encoder->setRenderPipelineState(billboardPipeline);
    for (auto& batch : billboardBatches) {
        encoder->drawPrimitives(PrimitiveTypeTriangle, 0, 6, batch.count);
    }

    // Pass 2: Mesh particles (instanced)
    encoder->setRenderPipelineState(meshParticlePipeline);
    for (auto& batch : meshBatches) {
        bindMesh(batch.meshIndex);
        encoder->drawIndexedPrimitives(
            PrimitiveTypeTriangle,
            meshIndexCount,
            IndexTypeUInt32,
            meshIndexBuffer,
            0,
            batch.count  // instance count
        );
    }
}
```

### Mesh Particle Vertex Shader (Metal)

```metal
struct MeshParticleVertexIn {
    float3 position [[attribute(0)]];
    float3 normal [[attribute(1)]];
    float2 uv [[attribute(2)]];
};

vertex MeshParticleVertexOut meshParticleVertex(
    MeshParticleVertexIn in [[stage_in]],
    uint instanceID [[instance_id]],
    constant CameraData& camera [[buffer(0)]],
    device const Particle* particles [[buffer(1)]],
    constant EmitterParams* emitters [[buffer(2)]]
) {
    Particle p = particles[instanceID];
    EmitterParams emitter = emitters[p.emitterID];

    // 構建變換矩陣
    float4x4 rotation = quatToMatrix(p.rotation);
    float4x4 scale = float4x4(p.scale);
    float4x4 translation = translationMatrix(p.position);
    float4x4 model = translation * rotation * scale;

    // 如果啟用速度對齊
    if (emitter.alignToVelocity != 0 && length(p.velocity) > 0.001) {
        float3 forward = normalize(p.velocity);
        float3 up = float3(0, 1, 0);
        float3 right = normalize(cross(up, forward));
        up = cross(forward, right);
        // 重建 rotation matrix...
    }

    MeshParticleVertexOut out;
    out.position = camera.proj * camera.view * model * float4(in.position, 1.0);
    out.normal = (rotation * float4(in.normal, 0.0)).xyz;
    out.uv = in.uv;
    out.color = p.color;

    return out;
}
```

### 優點

- 實現簡單，改動最小
- 粒子數據統一管理
- 容易擴展新的渲染模式

### 缺點

- `GPUParticle` 結構膨脹（Billboard 不需要 rotation/scale）
- 所有粒子佔用相同記憶體，即使不需要某些欄位
- 難以支援差異很大的粒子類型（如帶骨骼的 mesh）

---

## 方案 B：抽象 Renderer 介面（Niagara 風格）

### 設計概念

將渲染邏輯抽象成獨立的 Renderer 類別，每個 Emitter 指定使用哪個 Renderer。這是 Unreal Niagara 採用的架構。

```
┌─────────────────────────────────────────────────────────────┐
│                  ParticleSystem                              │
│  ┌─────────────────────────────────────────────────────────┐ │
│  │ GPUParticle Buffer (核心數據)                            │ │
│  │ position, velocity, life, emitterID                     │ │
│  └─────────────────────────────────────────────────────────┘ │
│                                                              │
│  ┌─────────────────────────────────────────────────────────┐ │
│  │ Renderer Registry                                        │ │
│  │  ├─ BillboardRenderer                                    │ │
│  │  ├─ MeshRenderer                                         │ │
│  │  ├─ RibbonRenderer                                       │ │
│  │  └─ (可擴展)                                              │ │
│  └─────────────────────────────────────────────────────────┘ │
└─────────────────────────────────────────────────────────────┘
```

### 類別設計

#### ParticleRenderer 基類

```cpp
// Vapor/include/Vapor/particle_renderer.hpp

class ParticleRenderer {
public:
    virtual ~ParticleRenderer() = default;

    // 返回此 renderer 需要的額外 per-particle 數據大小
    virtual size_t getExtraDataSize() const { return 0; }

    // 初始化 renderer 的 GPU 資源
    virtual void initialize(MTL::Device* device) = 0;

    // 渲染指定範圍的粒子
    virtual void render(
        MTL::RenderCommandEncoder* encoder,
        MTL::Buffer* particleBuffer,
        MTL::Buffer* extraDataBuffer,  // renderer 專用數據
        Uint32 startIndex,
        Uint32 count,
        const CameraData& camera
    ) = 0;

    // 返回 renderer 類型名稱（用於序列化）
    virtual const char* getTypeName() const = 0;
};
```

#### BillboardRenderer

```cpp
class BillboardRenderer : public ParticleRenderer {
public:
    size_t getExtraDataSize() const override { return 0; }  // 不需要額外數據

    void initialize(MTL::Device* device) override {
        // 創建 billboard pipeline
    }

    void render(...) override {
        encoder->setRenderPipelineState(billboardPipeline);
        encoder->drawPrimitives(PrimitiveTypeTriangle, 0, 6, count);
    }

    const char* getTypeName() const override { return "Billboard"; }

private:
    NS::SharedPtr<MTL::RenderPipelineState> billboardPipeline;
};
```

#### MeshRenderer

```cpp
// Mesh particles 需要的額外數據
struct MeshParticleData {
    glm::quat rotation;
    float scale;
    Uint32 meshIndex;
    float _pad[2];
};

class MeshRenderer : public ParticleRenderer {
public:
    size_t getExtraDataSize() const override {
        return sizeof(MeshParticleData);
    }

    void initialize(MTL::Device* device) override {
        // 創建 mesh particle pipeline
        // 載入預設 mesh 資源
    }

    void render(...) override {
        encoder->setRenderPipelineState(meshPipeline);
        encoder->setVertexBuffer(extraDataBuffer, 0, 3);  // 額外數據

        for (auto& mesh : meshes) {
            encoder->setVertexBuffer(mesh.vertexBuffer, 0, 4);
            encoder->drawIndexedPrimitives(..., count);
        }
    }

    void registerMesh(Uint32 index, const Mesh& mesh);

    const char* getTypeName() const override { return "Mesh"; }

private:
    NS::SharedPtr<MTL::RenderPipelineState> meshPipeline;
    std::unordered_map<Uint32, MeshHandle> meshes;
};
```

#### RibbonRenderer

```cpp
// Ribbon 需要連接相鄰粒子形成條帶
struct RibbonParticleData {
    Uint32 prevIndex;   // 前一個粒子的索引
    Uint32 nextIndex;   // 下一個粒子的索引
    float width;
    float texCoordU;
};

class RibbonRenderer : public ParticleRenderer {
public:
    size_t getExtraDataSize() const override {
        return sizeof(RibbonParticleData);
    }

    void render(...) override {
        // Ribbon 需要特殊的頂點生成邏輯
        // 可能需要 geometry shader 或 compute shader 預處理
    }

    const char* getTypeName() const override { return "Ribbon"; }
};
```

### 記憶體佈局

```
┌─────────────────────────────────────────────────────────────┐
│  Core Particle Buffer (所有粒子共用)                         │
│  [pos, vel, color, life, emitterID] × MAX_PARTICLES         │
└─────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────┐
│  Extra Data Buffers (per-renderer)                           │
│                                                              │
│  MeshRenderer Buffer:                                        │
│  [rotation, scale, meshIndex] × mesh_particle_count          │
│                                                              │
│  RibbonRenderer Buffer:                                      │
│  [prevIdx, nextIdx, width, u] × ribbon_particle_count        │
└─────────────────────────────────────────────────────────────┘
```

### Emitter 配置

```cpp
struct ParticleEmitterComponent {
    // 現有欄位...

    // Renderer 配置
    std::string rendererType = "Billboard";  // 或 "Mesh", "Ribbon"

    // Mesh Renderer 專用
    Uint32 meshIndex = 0;
    bool alignToVelocity = false;
    glm::vec3 meshScale = glm::vec3(1.0f);

    // Ribbon Renderer 專用
    float ribbonWidth = 0.1f;
    Uint32 ribbonSubdivisions = 4;
};
```

### ParticleSystem 整合

```cpp
class ParticleSystem {
public:
    void initialize() {
        // 註冊內建 renderers
        registerRenderer<BillboardRenderer>("Billboard");
        registerRenderer<MeshRenderer>("Mesh");
        registerRenderer<RibbonRenderer>("Ribbon");
    }

    template<typename T>
    void registerRenderer(const std::string& name) {
        renderers[name] = std::make_unique<T>();
        renderers[name]->initialize(device);
    }

    void render(entt::registry& reg) {
        // 按 renderer 類型分組 emitters
        std::unordered_map<std::string, std::vector<EmitterRenderInfo>> batches;

        auto view = reg.view<ParticleEmitterComponent>();
        for (auto entity : view) {
            auto& emitter = view.get<ParticleEmitterComponent>(entity);
            batches[emitter.rendererType].push_back({
                emitter.particleStartIndex,
                emitter.activeParticleCount
            });
        }

        // 每個 renderer 渲染自己的粒子
        for (auto& [type, infos] : batches) {
            auto& renderer = renderers[type];
            for (auto& info : infos) {
                renderer->render(encoder, particleBuffer, info.startIndex, info.count, camera);
            }
        }
    }

private:
    std::unordered_map<std::string, std::unique_ptr<ParticleRenderer>> renderers;
};
```

### 優點

- 高度模組化，易於擴展新的渲染類型
- 記憶體效率高（每種類型只存需要的數據）
- 符合 Niagara 等成熟引擎的設計
- 可以支援非常不同的粒子類型（甚至帶骨骼動畫）

### 缺點

- 實現複雜度高
- 需要管理多個 buffer
- Emitter 切換 renderer 時需要數據遷移

---

## 方案比較

| 特性 | 方案 A (共用 Buffer) | 方案 B (抽象 Renderer) |
|------|---------------------|----------------------|
| 實現複雜度 | 低 | 高 |
| 記憶體效率 | 中（結構膨脹） | 高（按需分配） |
| 擴展性 | 中 | 高 |
| 維護成本 | 低 | 中 |
| 支援複雜粒子類型 | 有限 | 良好 |
| 類似引擎 | Unity Shuriken | Unreal Niagara |

---

## 建議

### 短期（快速實現）
採用 **方案 A**：
1. 在 `GPUParticle` 加入 `rotation`, `scale`
2. 在 `EmitterParams` 加入 `renderMode`
3. 新增 `MeshParticlePipeline`
4. 修改 `ParticlePass` 支援兩種渲染模式

### 長期（完整系統）
遷移到 **方案 B**：
1. 抽象 `ParticleRenderer` 介面
2. 將現有 Billboard 邏輯封裝成 `BillboardRenderer`
3. 實現 `MeshRenderer`
4. （可選）實現 `RibbonRenderer` 支援拖尾效果

---

## 參考資料

- [Unreal Niagara 文檔](https://docs.unrealengine.com/5.0/en-US/niagara-visual-effects-in-unreal-engine/)
- [Unity VFX Graph](https://unity.com/visual-effect-graph)
- [GDC 2018: The Visual Effects of Destiny](https://www.youtube.com/watch?v=XxRFP-RjClo)

---

**文件版本**：v1.0
**最後更新**：2025-12-06
**作者**：Claude Code Assistant
