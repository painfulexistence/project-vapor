# GIBS (Global Illumination Based on Surfels) 設計文件

## 概述

GIBS 是一種基於表面元素（Surfel）的全局光照解決方案，適用於大場景和室內場景。本實作針對 Project Vapor 的 Metal 渲染架構設計。

## 設計決策記錄

### 決策 1：Surfel 資料結構大小

**問題**：Surfel 結構需要平衡資訊完整性和記憶體效率。

**選項**：
- A) 最小化 (64 bytes)：只存位置、法線、輻照度
- B) 標準 (128 bytes)：加入 albedo、半徑、時序資訊
- C) 完整 (256 bytes)：加入 SH 係數、多層材質

**決定**：選擇 B (128 bytes)

**理由**：
- 64 bytes 無法存儲足夠的時序穩定資訊
- 256 bytes 在 50 萬 surfels 時需要 128MB，過大
- 128 bytes 提供良好平衡，50 萬 surfels = 64MB

---

### 決策 2：空間哈希 vs 八叉樹

**問題**：如何快速查詢空間中的 surfels？

**選項**：
- A) 均勻空間哈希
- B) 八叉樹
- C) BVH

**決定**：選擇 A (均勻空間哈希)

**理由**：
- GPU 上建構和查詢最簡單
- 配合 parallel radix sort 可高效建構
- 對於 GI 這種「找附近 surfels」的查詢模式最有效
- 八叉樹建構複雜度高，BVH 更適合光線追蹤而非範圍查詢

---

### 決策 3：Surfel 生成策略

**問題**：何時、如何生成 surfels？

**選項**：
- A) 預處理：場景載入時一次性生成
- B) Runtime：從 G-buffer 動態生成
- C) 混合：靜態場景預生成 + 動態物體 runtime 補充

**決定**：選擇 C (混合策略)

**理由**：
- 純預處理無法處理動態物體
- 純 runtime 生成會造成時序不穩定（surfels 每幀都不同）
- 混合策略：靜態 surfels 提供穩定基礎，動態 surfels 處理移動物體

---

### 決策 4：每 Surfel 光線數量

**問題**：每個 surfel 每幀應該追蹤多少條光線？

**選項**：
- A) 1 ray/surfel/frame（極低品質）
- B) 4 rays/surfel/frame（低品質）
- C) 16 rays/surfel/frame（中品質）
- D) 64 rays/surfel/frame（高品質）

**決定**：選擇 B (4 rays)，配合時序累積

**理由**：
- 50 萬 surfels × 64 rays = 3200 萬條光線/幀，太慢
- 4 rays × 時序累積 8 幀 = 等效 32 rays，足夠收斂
- 可以提供品質設定讓用戶選擇

---

### 決策 5：螢幕採樣方式

**問題**：如何從 surfels 收集間接光到螢幕像素？

**選項**：
- A) 直接查詢：每像素查詢最近 N 個 surfels
- B) Screen-space probe：降解析度 probe 網格
- C) World-space interpolation：基於世界座標插值

**決定**：選擇 A (直接查詢) + 半解析度優化

**理由**：
- 直接查詢最準確
- 半解析度 (640x360 @ 1080p) 減少查詢次數
- 雙邊濾波上採樣保持邊緣銳利

---

## 架構圖

```
┌─────────────────────────────────────────────────────────────────┐
│                         Per Frame                                │
├─────────────────────────────────────────────────────────────────┤
│                                                                  │
│  ┌──────────────┐     ┌──────────────┐     ┌──────────────┐    │
│  │   PrePass    │────▶│  TLASBuild   │────▶│   MainPass   │    │
│  │  (Depth+N)   │     │              │     │    (PBR)     │    │
│  └──────────────┘     └──────────────┘     └──────────────┘    │
│         │                    │                    ▲             │
│         ▼                    ▼                    │             │
│  ┌──────────────┐     ┌──────────────┐     ┌──────────────┐    │
│  │   Surfel     │────▶│   Surfel     │────▶│    GIBS      │    │
│  │  Generation  │     │  Raytracing  │     │   Sample     │────┘
│  └──────────────┘     └──────────────┘     └──────────────┘    │
│         │                    │                    ▲             │
│         ▼                    ▼                    │             │
│  ┌──────────────┐     ┌──────────────┐           │             │
│  │   Spatial    │────▶│   Temporal   │───────────┘             │
│  │    Hash      │     │   Stability  │                         │
│  └──────────────┘     └──────────────┘                         │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘
```

## 記憶體預算

| 資源 | 大小 | 說明 |
|------|------|------|
| Surfel Buffer | 64 MB | 50萬 × 128 bytes |
| Cell Buffer | 16 MB | 稀疏哈希表 |
| Counter Buffer | 4 KB | Atomic counters |
| GI Result (half-res) | 2.6 MB | 960×540 × RGBA16F |
| **總計** | ~83 MB | |

## 效能目標

| 通道 | 目標時間 | 備註 |
|------|----------|------|
| Surfel Generation | < 0.5ms | 增量更新 |
| Spatial Hash | < 0.3ms | Parallel sort |
| Raytracing | < 3.0ms | 4 rays/surfel |
| Temporal | < 0.2ms | 簡單混合 |
| Screen Sample | < 0.5ms | 半解析度 |
| **總計** | < 4.5ms | @ 1080p, M1 Pro |

## 品質設定

```cpp
enum class GIBSQuality {
    Low,      // 25萬 surfels, 2 rays, 1/4 解析度採樣
    Medium,   // 50萬 surfels, 4 rays, 1/2 解析度採樣
    High,     // 100萬 surfels, 8 rays, 全解析度採樣
    Ultra     // 200萬 surfels, 16 rays, 全解析度採樣
};
```

## 已知限制

1. **透明物體**：Surfels 只能在不透明表面生成
2. **薄幾何**：非常薄的物體可能缺少 surfels
3. **動態光源**：光源移動時需要較長時間收斂
4. **遠距離**：超出空間哈希範圍的區域無 GI

## 實作摘要

### 新增檔案

| 路徑 | 說明 |
|------|------|
| `include/Vapor/gibs_manager.hpp` | Surfel 管理器頭文件 |
| `include/Vapor/gibs_passes.hpp` | Render passes 頭文件 |
| `src/gibs_manager.cpp` | Surfel 管理器實作 |
| `src/gibs_passes.cpp` | 所有 GIBS render passes |
| `assets/shaders/gibs_common.metal` | 共用結構和函數 |
| `assets/shaders/gibs_surfel_generation.metal` | Surfel 生成 |
| `assets/shaders/gibs_spatial_hash.metal` | 空間哈希建構 |
| `assets/shaders/gibs_raytracing.metal` | RT 光傳輸 |
| `assets/shaders/gibs_temporal.metal` | 時序穩定 |
| `assets/shaders/gibs_sample.metal` | 螢幕採樣 |

### 修改檔案

| 路徑 | 修改內容 |
|------|----------|
| `include/Vapor/graphics.hpp` | 新增 Surfel、GIBSData 等資料結構 |
| `include/Vapor/renderer_metal.hpp` | 新增 GIBS 成員變數和 pipelines |
| `src/renderer_metal.cpp` | 初始化 GIBS、添加 passes |
| `assets/shaders/3d_pbr_normal_mapped.metal` | 整合 GI 結果 |

### Render Graph 順序

```
PrePass → TLASBuild → NormalResolve → TileCulling → RaytraceShadow → RaytraceAO
    → SurfelGeneration → SurfelHashBuild → SurfelRaytracing → GIBSTemporal → GIBSSample
    → MainRender (使用 GI 結果) → ...
```

## 未來擴展

1. **多重反彈**：目前只實作單次反彈，可擴展到多次
2. **Specular GI**：加入粗糙反射的間接高光
3. **Volumetric GI**：與體積霧系統整合
4. **LOD 系統**：遠處使用更大、更少的 surfels

---

*文件版本：1.0*
*最後更新：2024-01*
