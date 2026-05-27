# Vulkan Backend Setup Guide

## 🎯 目標

讓 Vulkan 後端完全運作，與 Metal 後端功能一致。

## ✅ 現狀

### 已完成
- ✅ GLSL shaders 已經存在（15 個）
- ✅ renderer_vulkan.cpp 完整實作（173 KB）
- ✅ 所有渲染邏輯都已完成

### 只缺
- ❌ SPIR-V 編譯檔案（.spv）

## 🚀 快速開始

### 步驟 1：安裝 Vulkan SDK

#### macOS
```bash
brew install vulkan-sdk
```

#### Linux (Ubuntu/Debian)
```bash
sudo apt install glslang-tools vulkan-tools
```

#### Linux (Arch)
```bash
sudo pacman -S vulkan-tools glslang
```

#### 或從官網下載
https://vulkan.lunarg.com/sdk/home

### 步驟 2：編譯 Shaders

```bash
# 在專案根目錄執行
./compile_shaders.sh
```

輸出應該類似：
```
=== Compiling GLSL Shaders to SPIR-V ===
Shader directory: Vapor/assets/shaders
Output directory: Vapor/assets/shaders

Using glslangValidator: /usr/local/bin/glslangValidator

--- Core Rendering Shaders ---
Compiling: Vapor/assets/shaders/TBN.vert -> Vapor/assets/shaders/TBN.vert.spv
  ✓ Success
Compiling: Vapor/assets/shaders/PBRNormalMapped.frag -> Vapor/assets/shaders/PBRNormalMapped.frag.spv
  ✓ Success
...

✓ All shaders compiled successfully!
```

### 步驟 3：驗證編譯結果

```bash
# 檢查是否生成了 15 個 .spv 檔案
ls -la Vapor/assets/shaders/*.spv | wc -l
# 應該顯示: 15
```

### 步驟 4：建構專案

```bash
# 如果還沒有 build 目錄
cmake -B build -S .

# 建構
cmake --build build
```

### 步驟 5：執行 Vulkan 後端

```bash
# 使用 Vulkan 後端運行
./build/Vapor --vulkan

# 或使用 Metal 後端（macOS only）
./build/Vapor --metal
```

## 📋 已編譯的 Shaders 清單

編譯腳本會生成以下 15 個 SPIR-V 檔案：

### 核心渲染（6 個）
1. `TBN.vert.spv` - 主要頂點 shader（TBN 矩陣計算）
2. `PBRNormalMapped.frag` - PBR 片段 shader（法線貼圖）
3. `PrePass.vert.spv` - 深度 pre-pass 頂點 shader
4. `PrePass.frag.spv` - 深度 pre-pass 片段 shader
5. `FullScreen.vert.spv` - 全螢幕三角形頂點 shader
6. `PostProcess.frag.spv` - 後處理片段 shader

### Compute Shaders（1 個）
7. `TileLightCull.comp.spv` - Tile-based 光源剔除

### 粒子系統（4 個）
8. `ParticleForce.comp.spv` - 粒子力計算
9. `ParticleIntegrate.comp.spv` - 粒子積分
10. `Particle.vert.spv` - 粒子頂點 shader
11. `Particle.frag.spv` - 粒子片段 shader

### UI 渲染（2 個）
12. `RmlUi.vert.spv` - RmlUI 頂點 shader
13. `RmlUi.frag.spv` - RmlUI 片段 shader

### 2D 批次渲染（2 個）
14. `Batch2D.vert.spv` - 2D 批次頂點 shader
15. `Batch2D.frag.spv` - 2D 批次片段 shader

## 🐛 故障排除

### 問題 1：glslangValidator not found

**症狀：**
```
ERROR: glslangValidator not found!
```

**解決方案：**
安裝 Vulkan SDK（見步驟 1）

### 問題 2：Shader 編譯錯誤

**症狀：**
```
Compiling: Vapor/assets/shaders/PBRNormalMapped.frag -> ...
  ✗ Failed
ERROR: ... : compilation errors
```

**解決方案：**
1. 檢查 GLSL shader 語法
2. 確認 #version 450
3. 檢查 binding 和 set 編號

### 問題 3：執行時找不到 shader 檔案

**症狀：**
```
Failed to load shader: shaders/TBN.vert.spv
```

**解決方案：**
1. 確認 working directory 正確
2. 從專案根目錄執行
3. 檢查 shader 路徑配置

### 問題 4：Vulkan validation errors

**症狀：**
控制台顯示 Vulkan validation layer 錯誤

**解決方案：**
1. 檢查 descriptor set 綁定
2. 確認 pipeline layout 正確
3. 查看具體錯誤訊息並修正

## 🎮 執行參數

```bash
# 顯示幫助
./build/Vapor --help

# 使用 Vulkan 後端
./build/Vapor --vulkan

# 使用 Metal 後端（macOS only）
./build/Vapor --metal

# 設定視窗大小
./build/Vapor --vulkan --width 1920 --height 1080
```

## 📊 功能對比

### Metal 後端
- ✅ 完整 PBR 渲染
- ✅ Multi-pass rendering
- ✅ Clustered lighting
- ✅ **Ray traced shadows** ⭐ Metal 專屬
- ✅ **Ray traced AO** ⭐ Metal 專屬
- ✅ 2D/3D batch rendering
- ✅ 字型渲染
- ✅ Render-to-texture
- ✅ 後處理效果
- ✅ 粒子系統

### Vulkan 後端
- ✅ 完整 PBR 渲染
- ✅ Multi-pass rendering
- ✅ Clustered lighting
- ❌ Ray traced shadows（暫不支援）
- ❌ Ray traced AO（暫不支援）
- ✅ 2D/3D batch rendering
- ✅ 字型渲染
- ✅ Render-to-texture
- ✅ 後處理效果
- ✅ 粒子系統

## 🔬 測試清單

完成編譯後，請測試以下功能：

### Vulkan 後端測試
- [ ] 程式啟動無錯誤
- [ ] 場景正常顯示
- [ ] 幾何形狀正確
- [ ] 材質貼圖正確
- [ ] 光照效果正確（方向光 + 點光源）
- [ ] Clustered lighting 運作
- [ ] 攝影機控制正常
- [ ] ImGui UI 顯示正常
- [ ] 2D 批次渲染運作
- [ ] 粒子系統運作
- [ ] 後處理效果運作
- [ ] 效能可接受（60+ FPS）

### Metal 後端測試（對照組）
- [ ] 上述所有項目
- [ ] Ray traced shadows 運作
- [ ] Ray traced AO 運作

## ⏱️ 預期時間

- **編譯 shaders**: 1-2 分鐘
- **建構專案**: 3-5 分鐘
- **測試兩個後端**: 10-15 分鐘
- **總時間**: ~20 分鐘

## 📝 下一步

完成 Vulkan 後端設定後：

1. **正常開發**
   - 兩個後端都能用
   - 根據需求選擇後端
   - 繼續開發遊戲功能

2. **記錄維護經驗**（6 個月）
   - 兩個後端的維護成本
   - 是否經常出現不一致
   - 是否需要寫兩次新功能

3. **重新評估**（6 個月後）
   - 如果維護痛點明確 → 考慮重構為 RHI
   - 如果還好 → 繼續獨立後端

## 🎯 成功指標

完成後應該達到：

- ✅ 兩個後端都能運作
- ✅ 功能基本一致（除了 ray tracing）
- ✅ 效能差異 < 10%
- ✅ 可以隨時切換後端測試
- ✅ 沒有 validation errors

## 📚 參考資源

- **Vulkan Tutorial**: https://vulkan-tutorial.com/
- **Vulkan SDK Documentation**: https://vulkan.lunarg.com/doc/sdk
- **GLSL Reference**: https://www.khronos.org/opengl/wiki/OpenGL_Shading_Language
- **SPIR-V Tools**: https://github.com/KhronosGroup/SPIRV-Tools

---

## 💬 需要幫助？

如果遇到問題：
1. 檢查「故障排除」章節
2. 查看 Vulkan validation layer 輸出
3. 比對 Metal 後端行為
4. 檢查 shader 編譯錯誤訊息

**準備好了嗎？** 執行 `./compile_shaders.sh` 開始！ 🚀
