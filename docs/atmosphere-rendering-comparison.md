# 大氣渲染與 IBL 方案比較

本文檔比較三種常見的大氣渲染與 IBL（Image-Based Lighting）整合方案。

## 目錄

- [方案概覽](#方案概覽)
- [方案一：預計算 LUT](#方案一預計算-lut)
- [方案二：Cubemap 共用](#方案二cubemap-共用)
- [方案三：獨立計算](#方案三獨立計算)
- [效能比較](#效能比較)
- [選擇建議](#選擇建議)
- [參考資料](#參考資料)

---

## 方案概覽

| 方案 | 代表實作 | 複雜度 | 效能 | 適用場景 |
|------|---------|--------|------|---------|
| 預計算 LUT | UE4/UE5, Bruneton | 高 | 極佳 | AAA 遊戲、主機平台 |
| Cubemap 共用 | 多數獨立引擎 | 中 | 良好 | 一般專案、原型開發 |
| 獨立計算 | 簡易實作 | 低 | 中等 | 學習用途、靜態天空 |

---

## 方案一：預計算 LUT

### 核心概念

將大氣散射的物理計算結果預先存入查找表（Look-Up Table），運行時只需採樣 LUT 即可獲得天空顏色，避免即時 ray marching。

### 架構流程

```
┌─────────────────────────────────────────────────────────────┐
│  階段一：預計算物理 LUT（啟動時 / 大氣參數改變時）              │
├─────────────────────────────────────────────────────────────┤
│                                                             │
│  輸入：大氣參數（Rayleigh係數、Mie係數、行星半徑等）           │
│                                                             │
│  ┌─────────────────────┐    ┌─────────────────────┐        │
│  │ Transmittance LUT   │    │ Scattering LUT      │        │
│  │ (2D: 256×64)        │    │ (3D/4D: 256×128×32) │        │
│  │                     │    │                     │        │
│  │ 索引：              │    │ 索引：              │        │
│  │ - 高度 h            │    │ - 高度 h            │        │
│  │ - 天頂角 θ          │    │ - 太陽天頂角 θ_sun  │        │
│  │                     │    │ - 視線天頂角 θ_view │        │
│  │ 儲存：              │    │ - (方位角差 φ)      │        │
│  │ - RGB 透射率        │    │                     │        │
│  │                     │    │ 儲存：              │        │
│  │                     │    │ - RGB 散射量        │        │
│  └─────────────────────┘    └─────────────────────┘        │
│                                                             │
│  可選：Multi-Scattering LUT（多次散射預計算）                 │
│                                                             │
└─────────────────────────────────────────────────────────────┘
                              ↓
┌─────────────────────────────────────────────────────────────┐
│  階段二：生成 Sky Cubemap（太陽方向改變時）                    │
├─────────────────────────────────────────────────────────────┤
│                                                             │
│  for each cubemap_face (0..5):                              │
│      for each pixel (u, v):                                 │
│          direction = uv_to_direction(face, u, v)            │
│          color = sample_LUTs(T, S, sun_dir, direction)      │
│          cubemap[face][u][v] = color                        │
│                                                             │
│  輸出：Sky Cubemap (512×512×6, HDR)                         │
│                                                             │
│  注意：此步驟只需 LUT 採樣，不需 ray marching                 │
│                                                             │
└─────────────────────────────────────────────────────────────┘
                              ↓
┌─────────────────────────────────────────────────────────────┐
│  階段三：IBL 預處理（Sky Cubemap 更新後）                     │
├─────────────────────────────────────────────────────────────┤
│                                                             │
│  ┌─────────────────┐                                        │
│  │ Sky Cubemap     │                                        │
│  └────────┬────────┘                                        │
│           │                                                 │
│     ┌─────┴─────┬─────────────┐                             │
│     ↓           ↓             ↓                             │
│  ┌──────┐  ┌──────────┐  ┌──────────┐                      │
│  │Irrad.│  │Prefilter │  │BRDF LUT  │                      │
│  │Map   │  │Map       │  │(一次性)  │                      │
│  │32³   │  │128³×5mip │  │512²      │                      │
│  └──────┘  └──────────┘  └──────────┘                      │
│                                                             │
│  Irradiance Map：對半球積分，用於漫反射 IBL                   │
│  Prefilter Map：依粗糙度預濾波，用於鏡面反射 IBL              │
│  BRDF LUT：Schlick-GGX BRDF 積分表，只需計算一次             │
│                                                             │
└─────────────────────────────────────────────────────────────┘
                              ↓
┌─────────────────────────────────────────────────────────────┐
│  階段四：每幀渲染                                            │
├─────────────────────────────────────────────────────────────┤
│                                                             │
│  天空背景：                                                  │
│    方式 A：直接採樣 LUT → 螢幕（最高品質）                    │
│    方式 B：採樣 Sky Cubemap → 螢幕（較快）                   │
│                                                             │
│  場景物件 PBR 著色：                                         │
│    diffuse_ibl  = irradiance_map.sample(normal)             │
│    specular_ibl = prefilter_map.sample(reflect, roughness)  │
│                   * brdf_lut.sample(NdotV, roughness)       │
│    ambient = diffuse_ibl + specular_ibl                     │
│                                                             │
└─────────────────────────────────────────────────────────────┘
```

### LUT 採樣公式

```glsl
// 簡化版 LUT 採樣（實際實作需要更複雜的參數化）
vec3 sampleSkyColor(vec3 viewDir, vec3 sunDir, float height) {
    float cosViewZenith = dot(viewDir, up);
    float cosSunZenith = dot(sunDir, up);
    float cosViewSun = dot(viewDir, sunDir);

    // 從 LUT 採樣透射率
    vec3 transmittance = textureLUT_T(height, cosViewZenith);

    // 從 LUT 採樣散射
    vec3 scattering = textureLUT_S(height, cosSunZenith, cosViewZenith, cosViewSun);

    return scattering; // 已包含透射率加權
}
```

### 優缺點

**優點：**
- 每幀渲染極快（只需紋理採樣）
- 支持即時日夜循環而不卡頓
- 物理精確度高（LUT 可用高精度預計算）
- 適合主機/手機等算力受限平台

**缺點：**
- 實作複雜度高
- 大氣參數改變需重建 LUT（數秒）
- LUT 佔用顯存（數 MB）
- 需要理解大氣物理模型才能正確參數化

---

## 方案二：Cubemap 共用

### 核心概念

使用 ray marching 渲染大氣到 cubemap，此 cubemap 同時用於：
1. 天空背景顯示
2. IBL 環境光計算

避免重複計算大氣。

### 架構流程

```
┌─────────────────────────────────────────────────────────────┐
│  階段一：渲染天空到 Cubemap（參數改變時）                      │
├─────────────────────────────────────────────────────────────┤
│                                                             │
│  for each cubemap_face (0..5):                              │
│      for each pixel (u, v):                                 │
│          direction = uv_to_direction(face, u, v)            │
│          color = ray_march_atmosphere(direction, sun_dir)   │
│          cubemap[face][u][v] = color                        │
│                                                             │
│  輸出：Environment Cubemap (512×512×6, HDR)                 │
│                                                             │
│  成本：~128 次計算/像素（16 primary × 8 secondary steps）    │
│  總計：512 × 512 × 6 × 128 ≈ 2 億次計算                     │
│                                                             │
└─────────────────────────────────────────────────────────────┘
                              ↓
┌─────────────────────────────────────────────────────────────┐
│  階段二：IBL 預處理                                          │
├─────────────────────────────────────────────────────────────┤
│                                                             │
│  ┌───────────────────┐                                      │
│  │ Environment       │                                      │
│  │ Cubemap           │                                      │
│  └─────────┬─────────┘                                      │
│            │                                                │
│      ┌─────┴─────┬─────────────┐                            │
│      ↓           ↓             ↓                            │
│  ┌──────┐   ┌──────────┐  ┌──────────┐                     │
│  │Irrad.│   │Prefilter │  │BRDF LUT  │                     │
│  │Map   │   │Map       │  │          │                     │
│  └──────┘   └──────────┘  └──────────┘                     │
│                                                             │
└─────────────────────────────────────────────────────────────┘
                              ↓
┌─────────────────────────────────────────────────────────────┐
│  階段三：每幀渲染                                            │
├─────────────────────────────────────────────────────────────┤
│                                                             │
│  天空背景：                                                  │
│    color = environment_cubemap.sample(view_direction)       │
│    （直接從 cubemap 採樣，不重新計算）                        │
│                                                             │
│  場景物件：                                                  │
│    使用 Irradiance Map + Prefilter Map + BRDF LUT          │
│                                                             │
└─────────────────────────────────────────────────────────────┘
```

### 優缺點

**優點：**
- 實作相對簡單
- 大氣只計算一次，IBL 和天空共用
- 不需要預計算 LUT
- 參數改變可即時更新（但有延遲）

**缺點：**
- 天空解析度受限於 cubemap（512×512 可能不夠細緻）
- 太陽盤可能因 cubemap 解析度而模糊
- 更新 cubemap 時有明顯計算成本
- 不適合每幀更新（如快速日夜循環）

### 天空解析度問題的解決方案

```glsl
// 混合方案：cubemap + 即時太陽盤
vec3 skyColor = environment_cubemap.sample(viewDir);

// 太陽盤單獨即時計算（避免 cubemap 解析度不足）
float sunDot = dot(viewDir, sunDirection);
float sunDisk = smoothstep(0.9995, 0.9999, sunDot);
skyColor += sunColor * sunIntensity * sunDisk;
```

---

## 方案三：獨立計算

### 核心概念

IBL 烘焙和天空渲染各自獨立計算大氣，不共用結果。這是最簡單但效率最低的方案。

### 架構流程

```
┌─────────────────────────────────────────────────────────────┐
│  IBL 管線（參數改變時）                                       │
├─────────────────────────────────────────────────────────────┤
│                                                             │
│  Sky Capture Pass:                                          │
│      ray_march_atmosphere() → Environment Cubemap           │
│                                                             │
│  IBL Passes:                                                │
│      Environment Cubemap → Irradiance Map                   │
│      Environment Cubemap → Prefilter Map                    │
│      (BRDF LUT 一次性計算)                                   │
│                                                             │
└─────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────┐
│  每幀渲染                                                    │
├─────────────────────────────────────────────────────────────┤
│                                                             │
│  Main Render Pass:                                          │
│      渲染場景物件，使用 IBL maps                             │
│                                                             │
│  Sky Atmosphere Pass:                                       │
│      ray_march_atmosphere() → 螢幕  ← 又算了一次！           │
│                                                             │
└─────────────────────────────────────────────────────────────┘
```

### 優缺點

**優點：**
- 實作最簡單
- 天空渲染可以是全螢幕解析度
- IBL 和天空可以使用不同參數（如果需要）

**缺點：**
- 大氣計算做了兩次（浪費）
- 每幀都要完整 ray march 天空
- 不適合效能敏感的應用

---

## 效能比較

### 理論計算成本

假設條件：
- Cubemap 解析度：512×512×6
- 螢幕解析度：1920×1080
- Ray march：16 primary steps × 8 secondary steps = 128 ops/pixel
- LUT 採樣：4 texture fetches/pixel

| 方案 | Sky Cubemap 生成 | 每幀天空渲染 | 總計（參數不變時） |
|------|-----------------|-------------|-------------------|
| LUT | ~400萬 ops | ~800萬 ops | **~800萬 ops/幀** |
| Cubemap 共用 | ~2億 ops | ~200萬 ops | **~200萬 ops/幀** |
| 獨立計算 | ~2億 ops | ~2.6億 ops | **~2.6億 ops/幀** |

### 更新頻率影響

| 方案 | 參數不變 | 每秒更新 | 每幀更新 |
|------|---------|---------|---------|
| LUT | 極佳 | 不可行 | 不可行 |
| Cubemap 共用 | 良好 | 可接受 | 卡頓 |
| 獨立計算 | 中等 | 中等 | 中等 |

---

## 選擇建議

### 選擇方案一（LUT）當：

- 需要即時日夜循環（太陽持續移動）
- 目標平台算力有限（主機、手機）
- 專案規模較大，值得投入實作成本
- 需要最高的物理精確度

### 選擇方案二（Cubemap 共用）當：

- 大氣參數不常改變
- 不需要極高的天空解析度
- 想要簡單實作又有合理效能
- 大多數遊戲專案的推薦選擇

### 選擇方案三（獨立計算）當：

- 原型開發或學習用途
- 天空是靜態的
- 專案對效能要求不高
- 需要 IBL 和天空使用不同參數

---

## 實作檢查清單

### 從方案三升級到方案二

- [ ] 修改 `SkyAtmospherePass` 改為採樣 `environmentCubemap`
- [ ] 建立 `3d_skybox.metal` shader（簡單的 cubemap 採樣）
- [ ] 保留即時太陽盤渲染（避免 cubemap 解析度問題）
- [ ] 調整渲染順序確保 cubemap 在使用前已更新

### 從方案二升級到方案一

- [ ] 實作 Transmittance LUT 預計算
- [ ] 實作 Scattering LUT 預計算
- [ ] 實作 LUT 參數化（高度、角度映射）
- [ ] 修改 sky capture 改用 LUT 採樣
- [ ] 實作 LUT 重建觸發機制
- [ ] 考慮 multi-scattering LUT（可選）

---

## 參考資料

1. **Bruneton, Eric.** "Precomputed Atmospheric Scattering" (2008)
   - 原始 LUT 方法論文
   - https://hal.inria.fr/inria-00288758

2. **Hillaire, Sébastien.** "A Scalable and Production Ready Sky and Atmosphere Rendering Technique" (2020)
   - UE4/UE5 的大氣系統
   - EGSR 2020

3. **cpp-rendering.io** "Sky and Atmosphere Rendering"
   - 實作導向的教學
   - https://cpp-rendering.io/sky-and-atmosphere-rendering/

4. **LearnOpenGL** "IBL - Diffuse Irradiance" & "IBL - Specular IBL"
   - IBL 基礎實作教學
   - https://learnopengl.com/PBR/IBL/Diffuse-irradiance

---

*文檔版本：1.0*
*最後更新：2024*
