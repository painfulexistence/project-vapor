# RHI Branch Strategy Decision

## 📅 決策日期
2026-05-28

## 🎯 決策：Option A - 繼續 RHI 重構

### 背景
嘗試 merge main 分支時發現 60+ 個檔案衝突，原因是兩個分支的架構根本不同：
- **Main 分支**: Renderer 作為虛擬介面，Metal/Vulkan 各自完整實作
- **RHI 分支**: RHI 作為抽象層，Renderer 為具體類別使用 RHI

### 決策理由
1. **已投入工作**: ~20 小時，完成 45%，不應放棄
2. **架構優勢**: RHI 分層更清晰，程式碼重用性高
3. **可擴展性**: 容易支援新後端（D3D12, WebGPU）
4. **原始問題**: Main 分支確實存在重複程式碼問題（已在分析文件中確認）

### 策略

#### 1. 不強制 merge main
- 保持 RHI 分支獨立開發
- 避免複雜的衝突解決
- 最終以 PR 形式整合

#### 2. 選擇性移植 main 的功能
從 main 分支手動移植需要的部分：
- Scene/ECS component 定義
- Sprite system 和 Flipbook components
- 更完整的材質系統
- Font rendering 實作細節
- 任何新增的實用功能

#### 3. 完成 RHI 架構
按照 RHI_PHASE_PLAN.md 繼續：
- Phase 1: 架構統一 (90% → 100%)
- Phase 2: 批次渲染完善
- Phase 3: 字型渲染
- Phase 4: Render-to-Texture
- Phase 5: 後處理效果
- Phase 6: RHI 實作完善 (基本完成)
- Phase 7: 清理與測試

## 📋 需要從 Main 移植的功能

### 高優先級（立即需要）
1. **Scene components**
   - `TransformComponent`
   - `MeshRendererComponent`
   - 其他 ECS components

2. **Sprite system**
   - `SpriteComponent`
   - `FlipbookComponent`
   - Sprite rendering logic

### 中優先級（Phase 2-3）
3. **材質系統改進**
   - 更完整的 material parameters
   - Material batching logic

4. **Font rendering 細節**
   - Font atlas generation
   - Glyph caching
   - Text layout

### 低優先級（Phase 4-5）
5. **後處理改進**
   - 更多效果選項
   - Effect pipeline

6. **其他新功能**
   - 任何在 main 上新增的實用工具

## 🚫 不移植的部分

1. **renderer_metal.cpp / renderer_vulkan.cpp**
   - 這些是單體實作，與 RHI 架構衝突
   - 我們有 rhi_metal.cpp / rhi_vulkan.cpp + renderer.cpp 代替

2. **舊的介面設計**
   - Main 的 virtual Renderer base class
   - 我們使用 RHI interface 代替

## 📊 時間估計

### 剩餘工作量
- Phase 1 完成: 1-2 小時
- Phase 2-3: 6-8 小時
- Phase 4-5: 4-6 小時
- Phase 7: 2-3 小時
- 從 main 移植功能: 4-6 小時
- **總計: 17-25 小時**

### 完成時間軸
假設每天工作 4 小時：
- **最快**: 4-5 天
- **預期**: 6-7 天
- **保守**: 8-10 天

## 🎯 成功指標

### Phase 完成指標
- [ ] Phase 1: 所有 API 有實作，測試通過
- [ ] Phase 2: Batch rendering 完全運作（texture array, auto-flush）
- [ ] Phase 3: Font rendering 運作
- [ ] Phase 4: RTT 可用於材質和 UI
- [ ] Phase 5: 後處理效果運作
- [ ] Phase 7: 所有測試通過，文件完整

### 品質指標
- [ ] Metal 後端完全運作
- [ ] Vulkan 後端完全運作（除 ray tracing）
- [ ] 功能與 main 分支對等
- [ ] 程式碼重複少於 main 分支
- [ ] 效能差異 < 5%

## 🔄 最終整合計畫

### 時機
- 當 RHI 分支功能完整且穩定時
- 通過所有測試
- 文件完整

### 方式
1. 創建 PR: `feat: Add RHI abstraction layer`
2. 詳細說明架構改變
3. 提供 migration guide
4. 展示優勢（程式碼減少、可擴展性）

### 預期反應
- Main 分支需要調整才能接受
- 可能需要提供 backward compatibility
- 可能需要分階段整合

## 📝 備註

這個決策基於：
1. 技術分析（ANALYSIS.md, RHI_TRADEOFF_ANALYSIS.md）
2. 實際衝突情況（60+ 個檔案）
3. 已投入的工作（20 小時）
4. 長期維護考量

如果未來情況改變（例如 main 分支也重構為 RHI 架構），可以重新評估策略。

---

**決策者**: User + Claude  
**執行**: Claude  
**審查**: 定期檢視進度，確保方向正確
