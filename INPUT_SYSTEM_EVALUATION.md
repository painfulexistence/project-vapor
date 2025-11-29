# Input System Evaluation and Implementation

## 評估結果

### AtmosphericEngine Input System 特點分析

**優點：**
- ✅ 狀態追蹤（當前幀 + 前一幀）支援 IsKeyPressed/Released
- ✅ Input buffer（記錄最近的按鍵事件）
- ✅ 提供 ImGui 調試介面
- ❌ 依賴 Window 抽象層（不符合需求）
- ❌ 直接暴露 Key enum，缺乏 action 映射

**缺點：**
- 無法重新綁定按鍵（硬編碼）
- 擴展性不佳（添加新輸入源困難）
- 與具體按鍵耦合過緊

### Python InputManager 設計優點

**優點：**
- ✅ Action-based 抽象（key → action 映射）
- ✅ InputState 物件清晰分離狀態
- ✅ 支援 held/pressed/released 語意
- ✅ 提供輔助方法（如 get_movement_vector）

## 實作的新 Input System

### 設計目標

結合兩者優點，為 project-vapor 設計了：
1. **Action-based 抽象** - 不暴露原始按鍵，易於重新綁定
2. **Input buffer** - 記錄最近的輸入事件（可用於連擊檢測、調試）
3. **State tracking** - 支援 held/pressed/released 查詢
4. **直接處理 SDL 事件** - 無需 Window 抽象層
5. **整合到 EngineCore** - 與其他核心系統一致

### 架構概覽

```
InputManager (in EngineCore)
    ├─ InputAction enum (抽象的遊戲動作)
    ├─ InputState (當前幀的輸入狀態)
    │   ├─ held actions (持續按下)
    │   ├─ pressed actions (本幀剛按下)
    │   └─ released actions (本幀剛放開)
    ├─ Key → Action 映射表
    ├─ Input event buffer (最近 32 個事件)
    └─ Mouse position/delta
```

### 核心組件

#### 1. InputAction Enum
```cpp
enum class InputAction {
    MoveForward, MoveBackward,
    StrafeLeft, StrafeRight,
    MoveUp, MoveDown,
    LookUp, LookDown, LookLeft, LookRight,
    RollLeft, RollRight,
    SwitchToFlyCam, SwitchToFollowCam,
    Jump, Crouch, Sprint, Interact, Cancel,
    // ...
};
```

#### 2. InputState Class
```cpp
class InputState {
    bool isHeld(InputAction action);      // 持續按住
    bool isPressed(InputAction action);    // 本幀剛按下
    bool isReleased(InputAction action);   // 本幀剛放開
    glm::vec2 getMovementVector(...);      // 方向輔助
};
```

#### 3. InputManager
```cpp
class InputManager {
    void processEvent(const SDL_Event& event);  // 處理事件
    void update(float deltaTime);                // 每幀更新
    const InputState& getInputState();           // 獲取狀態
    void mapKey(SDL_Scancode, InputAction);      // 按鍵映射
    // Input buffer, mouse tracking...
};
```

### 整合情況

#### EngineCore 整合
```cpp
class EngineCore {
    InputManager& getInputManager();
    // 在 init() 中初始化
    // 在 update() 中更新（清除 per-frame 狀態）
};
```

#### CameraManager 更新
```cpp
// 之前：
void update(float deltaTime, const std::unordered_map<SDL_Scancode, bool>& keys);

// 現在：
void update(float deltaTime, const InputState& inputState);
```

#### Main Loop 簡化
```cpp
// 之前：
std::unordered_map<SDL_Scancode, bool> keyboardState;
while (SDL_PollEvent(&e)) {
    if (e.type == SDL_EVENT_KEY_DOWN) keyboardState[e.key.scancode] = true;
    // 手動處理每個按鍵...
}
cameraManager.update(dt, keyboardState);

// 現在：
auto& inputManager = engineCore->getInputManager();
while (SDL_PollEvent(&e)) {
    inputManager.processEvent(e);  // 一行搞定
}
engineCore->update(dt);
const auto& inputState = inputManager.getInputState();
if (inputState.isPressed(InputAction::SwitchToFlyCam)) { /* ... */ }
cameraManager.update(dt, inputState);
```

## 對架構清晰度的影響

### ✅ 優點

1. **責任分離更清晰**
   - InputManager 專注輸入處理
   - CameraManager 不需要知道 SDL 細節
   - Main loop 更簡潔

2. **可測試性提升**
   - 可以 mock InputState 來測試 Camera
   - 不依賴 SDL 事件系統

3. **可維護性提升**
   - 添加新動作：只需擴展 InputAction enum
   - 重新綁定按鍵：修改 loadDefaultMappings()
   - 支援不同輸入源（手把）：擴展 processEvent()

4. **與現有系統一致**
   - 遵循 EngineCore 的管理器模式
   - 與 ActionManager、ResourceManager 架構一致

5. **調試友好**
   - Input buffer 記錄最近事件
   - 可以輕鬆加入 ImGui 調試面板
   - 狀態查詢清晰（held vs pressed vs released）

### ⚠️ 注意事項

1. **學習曲線**
   - 需要理解 InputAction 枚舉
   - 需要了解 held/pressed/released 差異

2. **間接性**
   - 不能直接查詢 "W 鍵是否按下"
   - 需要透過 InputAction::MoveForward

3. **記憶體開銷**
   - Input buffer (32 events × 16 bytes = ~512 bytes)
   - Action sets (3 × hash_set，可忽略不計)

## 移植難度評估

### 從 AtmosphericEngine 移植：**中等**

- ✅ 核心概念相似（state tracking + buffer）
- ✅ API 易於理解
- ⚠️ 需要定義 InputAction 枚舉
- ⚠️ 需要建立 key mappings

### 從 Python InputManager 移植：**簡單**

- ✅ API 幾乎一對一對應
- ✅ InputState 概念完全相同
- ✅ 使用方式基本一致

## 建議

### 短期
1. ✅ 已完成基本實作
2. 🔄 測試編譯和運行
3. 📝 考慮加入 ImGui 調試面板（顯示 input buffer）

### 中期
1. 添加手把支援（GameController API）
2. 支援自訂按鍵綁定（序列化/反序列化）
3. 添加 axis 支援（搖桿、滑鼠移動）

### 長期
1. 輸入錄製/回放（用於測試）
2. 連擊檢測系統
3. 輸入優先級和衝突解決

## 結論

✅ **推薦採用此實作**

**理由：**
1. 架構清晰度顯著提升（-100 行雜亂的事件處理，+清晰的 action-based API）
2. 移植難度可控（中等，約 2-3 小時實作完成）
3. 與專案現有架構一致（EngineCore 管理器模式）
4. 擴展性強（易於添加新輸入源、重新綁定）
5. 保留了兩個參考實作的優點

**與 AtmosphericEngine 相比：**
- ✅ 保留了 state tracking 和 input buffer
- ✅ 移除了 Window 抽象依賴
- ✅ 增加了 action mapping 靈活性

**與 Python InputManager 相比：**
- ✅ API 設計幾乎相同
- ✅ C++ 類型安全 + 效能優勢
- ✅ 整合到引擎核心系統

## 檔案清單

### 新增檔案
- `Vapor/include/Vapor/input_manager.hpp` - InputManager、InputState、InputAction
- `Vapor/src/input_manager.cpp` - 實作

### 修改檔案
- `Vapor/include/Vapor/engine_core.hpp` - 加入 InputManager
- `Vapor/src/engine_core.cpp` - 初始化和更新 InputManager
- `Vaporware/src/camera_manager.hpp` - 使用 InputState 而非 SDL keyboard map
- `Vaporware/src/camera_manager.cpp` - 更新實作
- `Vaporware/src/main.cpp` - 簡化事件處理，使用 InputManager
- `Vapor/CMakeLists.txt` - 加入 input_manager.cpp 到編譯清單
