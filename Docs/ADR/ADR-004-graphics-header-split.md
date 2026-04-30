# ADR-004: graphics.hpp 拆分為語義子標頭

- **日期**：2026-04-29
- **狀態**：已採納

## 背景

`Vapor/include/Vapor/graphics.hpp` 原本是 543 LOC 的 God Header，包含：
- GPU handles（PipelineHandle、BufferHandle、TextureHandle、RenderTargetHandle）
- 2D batch 渲染結構（Batch2DVertex、Batch2DStats 等）
- 核心 GPU structs（MaterialData、CameraData、InstanceData 等）
- CPU-side mesh/material（Mesh、Material、Image）
- 視覺效果結構（Water、Atmosphere、Volumetric Fog/Cloud、God Rays、Sun Flare、Particles）

任何修改任一個結構都會觸發所有 14 個 include 此 header 的檔案重新編譯。

## 決策

將 `graphics.hpp` 拆分為五個語義子標頭，並保留 `graphics.hpp` 作為零破壞的傘形 header：

| 子標頭 | 內容 |
|--------|------|
| `graphics_handles.hpp` | `GPUHandle<Tag>` + 所有 handle typedef |
| `graphics_batch2d.hpp` | BlendMode、Batch2DVertex、Batch2DUniforms、Batch2DStats |
| `graphics_gpu_structs.hpp` | PrimitiveMode、MaterialData、lights、CameraData、InstanceData、Cluster、IBLCaptureData |
| `graphics_mesh.hpp` | AlphaMode、Image、Material、VertexData、Mesh |
| `graphics_effects.hpp` | Water、Atmosphere、VolumetricFog/Cloud、LightScattering、SunFlare、Particles |

## 附加決策：Handle 型別安全

原有的四個 handle（PipelineHandle、BufferHandle、TextureHandle、RenderTargetHandle）底層都是 `struct { Uint32 rid; }`，型別系統無法防止混用。

改用 `GPUHandle<Tag>` 模板：
```cpp
template<typename Tag>
struct GPUHandle { Uint32 rid = UINT32_MAX; bool valid() const; };
using BufferHandle  = GPUHandle<BufferHandleTag>;
using TextureHandle = GPUHandle<TextureHandleTag>;
// ...
```

現在把 `BufferHandle` 傳給需要 `TextureHandle` 的函式是編譯錯誤。

## 遷移策略

- 現有所有 `#include "graphics.hpp"` **不需要改動**——傘形 header 重新 export 所有定義
- `graphics.cpp`（只實作 Mesh 方法）改為 `#include "graphics_mesh.hpp"` 避免不必要的依賴
- 新增檔案優先 include 最小子標頭

## 影響

- 5 個新 header 檔案
- `graphics.hpp` 縮減為 15 行傘形 include
- Handle 類型不再可互換
- 未來新增視覺效果 struct 有明確的歸屬位置（`graphics_effects.hpp`）
