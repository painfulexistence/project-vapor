# Parity Matrix: Metal vs Vulkan Backends

> 分析日期：2026-05-01
> 來源：`renderer.hpp`（抽象介面）、`renderer_metal.hpp`（562 LOC）、`renderer_vulkan.hpp`（192 LOC）
> 凡例：✅ 完整 | ⚠️ 部分/disabled | ❌ 缺失/stub

---

## 抽象介面方法（renderer.hpp）

| 功能 | Metal | Vulkan | 備註 |
|------|-------|--------|------|
| `init(SDL_Window*)` | ✅ | ✅ | |
| `deinit()` | ✅ | ✅ | |
| `stage(Scene)` | ✅ | ✅ | |
| `draw(Scene, Camera)` | ✅ | ✅ | |
| `setRenderPath` / `getRenderPath` | ✅ | ✅ | |
| `createTexture(Image)` | ✅ | ✅ | |
| `initUI()` | ✅ | ❌ | Vulkan 無 RmlUI |
| `getDebugDraw()` | ✅ | ❌ | Vulkan 無 debug wireframe |
| `flush2D()` / `flush3D()` | ✅ | ❌ | Vulkan 無 batch flush |
| `drawQuad2D(...)` × 5 overloads | ✅ | ❌ | |
| `drawQuad3D(...)` × 4 overloads | ✅ | ❌ | |
| `drawRotatedQuad2D(...)` × 2 | ✅ | ❌ | |
| `drawLine2D` / `drawLine3D` | ✅ | ❌ | |
| `drawRect2D` | ✅ | ❌ | |
| `drawCircle2D` / `drawCircleFilled2D` | ✅ | ❌ | |
| `drawTriangle2D` / `drawTriangleFilled2D` | ✅ | ❌ | |
| `getBatch2DStats()` / `resetBatch2DStats()` | ✅ | ❌ | |
| `loadFont` / `unloadFont` | ✅ | ❌ | |
| `drawText2D` / `drawText3D` | ✅ | ❌ | |
| `measureText` / `getFontLineHeight` | ✅ | ❌ | |

---

## Render Passes

| Pass | 用途 | Metal | Vulkan | 關鍵 TODO |
|------|------|-------|--------|-----------|
| **IBL** | | | | |
| `SkyCapturePass` | 天空盒 cubemap capture | ✅ | ❌ | |
| `IrradianceConvolutionPass` | 漫反射 IBL | ✅ | ❌ | |
| `PrefilterEnvMapPass` | 鏡面反射 IBL | ✅ | ❌ | |
| `BRDFLUTPass` | BRDF 積分表 | ✅ | ❌ | |
| **主要 Pass** | | | | |
| `TLASBuildPass` | Ray tracing TLAS 建置 | ✅ | ❌ | Vulkan 無 RT pipeline |
| `PrePass` | 深度預渲染 | ✅ | ✅ | |
| `NormalResolvePass` | 法線重建 | ✅ | ❌ | |
| `TileCullingPass` | Light cluster culling compute | ✅ | ❌ | |
| `RaytraceShadowPass` | 光追硬陰影 | ✅ | ❌ | |
| `RaytraceAOPass` | 光追 AO | ✅ | ❌ | |
| `MainRenderPass` | PBR forward/deferred | ✅ | ✅ | Vulkan 僅 basic forward |
| `SkyAtmospherePass` | 大氣散射天空 | ✅ | ❌ | |
| **水面 / 粒子** | | | | |
| `WaterPass` | 水面渲染 | ⚠️ disabled | ❌ | renderer_metal.cpp line ~2456 已 comment out |
| `ParticlePass` | GPU 粒子 | ✅ | ⚠️ | Vulkan 有 particle 基礎但無完整 pipeline |
| **體積效果** | | | | |
| `VolumetricFogPass` | 體積霧 | ✅ | ❌ | |
| `VolumetricCloudPass` | 體積雲 | ✅ | ❌ | |
| `LightScatteringPass` | 丁達爾光束（god rays） | ✅ | ❌ | |
| **2D/3D Batch** | | | | |
| `WorldCanvasPass` | 3D batch（世界空間） | ✅ | ❌ | |
| `CanvasPass` | 2D batch（螢幕空間） | ✅ | ❌ | |
| **後處理** | | | | |
| `BloomBrightnessPass` | Bloom 亮度擷取 | ✅ | ❌ | |
| `BloomDownsamplePass` | Bloom downsample pyramid | ✅ | ❌ | |
| `BloomUpsamplePass` | Bloom upsample blur | ✅ | ❌ | |
| `BloomCompositePass` | Bloom 合成 | ✅ | ❌ | |
| `SunFlarePass` | 太陽鏡頭光暈 | ✅ | ❌ | |
| `DOFCoCPass` | 景深 CoC 計算 | ⚠️ disabled | ❌ | renderer_metal.cpp lines ~2479-2481 comment |
| `DOFBlurPass` | 景深模糊 | ⚠️ disabled | ❌ | |
| `DOFCompositePass` | 景深合成 | ⚠️ disabled | ❌ | |
| `PostProcessPass` | Tone mapping / color grading | ✅ | ✅ | Vulkan 僅基礎 |
| **UI / Debug** | | | | |
| `DebugDrawPass` | Wireframe 碰撞可視化 | ✅ | ❌ | |
| `RmlUiPass` | HTML/CSS HUD | ✅ | ❌ | |
| `ImGuiPass` | Developer debug overlay | ✅ | ❌ | |

---

## 總覽

| 類別 | Metal | Vulkan | 缺口 |
|------|-------|--------|------|
| 核心生命週期 | 6/6 | 6/6 | ✅ 完整同等 |
| Texture 管理 | 1/1 | 1/1 | ✅ 完整同等 |
| IBL Passes | 4/4 | 0/4 | ❌ 100% 缺失 |
| Ray Tracing Passes | 3/3 | 0/3 | ❌ 100% 缺失 |
| 體積效果 Passes | 3/3 | 0/3 | ❌ 100% 缺失 |
| 後處理 Passes | 9（其中 3 disabled） | 1/9 | ❌ 89% 缺失 |
| 2D/3D Batch API | 23/23 | 0/23 | ❌ 100% 缺失 |
| Font Rendering API | 6/6 | 0/6 | ❌ 100% 缺失 |
| UI Rendering | 2/2 | 0/2 | ❌ 100% 缺失 |
| Debug Drawing | 1/1 | 0/1 | ❌ 缺失 |

---

## 已知技術債（未修復項目）

### Vulkan ❌ 項目（全部待處理）
所有以上 ❌ 項目均未在 Vulkan 中實作。Vulkan 後端目前僅能完成基礎 3D 場景渲染（depth prepass → main forward pass → basic post-process），**不適合生產使用**。

若要修復 Vulkan 後端，優先順序建議：
1. 2D/3D batch API（影響最廣，許多 demo 功能依賴）
2. UI rendering（RmlUI + ImGui）
3. Bloom + basic post-process 補全
4. IBL（PBR 質量直接相關）
5. 體積效果 + 光追（最複雜，可最後處理）

### Metal ⚠️ 項目（disabled）
- **WaterPass**：Infrastructure 存在，pass 已 comment out（`renderer_metal.cpp` ~line 2456）
- **DOFCoCPass / DOFBlurPass / DOFCompositePass**：景深三 pass 均已 comment out（~lines 2479-2481）

這些功能在 FEATURES.md 中標記為存在，但實際上在 Metal 後端已停用。未修復前不應對外宣稱支援。
