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

---

## 為什麼選擇 Enum-based 而非 String-based？

### 常見疑問：使用者能自定義 InputAction 嗎？

**答案：不能直接添加新的 InputAction，但也不需要。**

### Enum vs String 比較

#### 當前實作（Enum-based）

```cpp
enum class InputAction {
    MoveForward,
    Jump,
    // 編譯時固定
};

// 使用
if (inputState.isPressed(InputAction::Jump)) { /* ... */ }
```

**優點：**
- ✅ **類型安全** - 編譯器會檢查拼寫錯誤
- ✅ **效能好** - 整數比較，hash 查找快速
- ✅ **IDE 支援** - 自動補全、重構友好
- ✅ **清晰** - 所有可用 action 一目了然
- ✅ **適合 99% 的遊戲需求**

**限制：**
- ❌ 無法在運行時動態添加新 action

#### String-based 替代方案

```cpp
using InputAction = std::string;

// 使用
if (inputState.isPressed("jump")) { /* ... */ }
if (inputState.isPressed("custom_mod_action_123")) { /* ... */ }
```

**優點：**
- ✅ 完全動態，可以在運行時創建任意 action

**缺點：**
- ❌ **沒有類型安全** - `"jump"` vs `"Jump"` vs `"jamp"` 都會編譯通過
- ❌ **效能較差** - string 比較、hash 計算開銷
- ❌ **容易出錯** - 拼寫錯誤只能在運行時發現
- ❌ **IDE 不友好** - 沒有自動補全
- ❌ **調試困難** - 錯誤的 string 不會報錯，只是靜默失敗

### 為什麼大多數遊戲不需要動態 Action？

#### 誤解：「自定義輸入 = 自定義 Action」

**實際上，玩家想要的是：**

1. **重新綁定按鍵** ✅ Enum 方案支援
   ```cpp
   // 預設：空格 = 跳躍
   inputManager.mapKey(SDL_SCANCODE_SPACE, InputAction::Jump);

   // 玩家改成：W = 跳躍
   inputManager.mapKey(SDL_SCANCODE_W, InputAction::Jump);
   ```

2. **修改靈敏度/參數** ✅ 與 Action 無關
   ```cpp
   camera.setSensitivity(0.5f);
   ```

**玩家不需要：**
- ❌ 憑空創造一個遊戲不支援的新動作
- ❌ 定義 `"my_custom_teleport_action"` 但遊戲邏輯不知道怎麼處理

#### 真實世界的例子

**Unreal Engine:**
- 使用預定義的 Action/Axis 名稱
- 在編輯器中配置，編譯後固定
- 玩家只能重新綁定按鍵，不能創造新 Action

**Unity Input System:**
- 使用 Input Action Asset（預定義）
- 支援重新綁定，但 Action 集合固定
- 即使是 MOD 也是擴展現有 Action，不是動態創建

**為什麼？**
- 每個 Action 都需要對應的遊戲邏輯
- 如果動態創建 `"teleport"` action，但遊戲沒有傳送功能，有什麼用？
- **Action 是遊戲設計的一部分，不是玩家配置的一部分**

### 當前方案已經足夠靈活

#### 重新綁定按鍵（運行時）

```cpp
// 玩家可以在遊戲中改變按鍵綁定
void rebindKey(SDL_Scancode oldKey, SDL_Scancode newKey) {
    auto* action = inputManager.getActionForKey(oldKey);
    if (action) {
        inputManager.unmapKey(oldKey);
        inputManager.mapKey(newKey, *action);
    }
}

// 使用
rebindKey(SDL_SCANCODE_SPACE, SDL_SCANCODE_W);  // 跳躍從空格改成 W
```

#### 序列化按鍵綁定

```cpp
// 保存玩家的自定義按鍵配置
nlohmann::json saveKeyBindings() {
    json config;
    for (auto action : getAllActions()) {
        config[actionToString(action)] = getKeysForAction(action);
    }
    return config;
}

// 載入
void loadKeyBindings(const json& config) {
    inputManager.clearMappings();
    for (auto& [actionName, keys] : config.items()) {
        auto action = stringToAction(actionName);
        for (auto key : keys) {
            inputManager.mapKey(key, action);
        }
    }
}
```

#### 添加新 Action（開發時）

只需要修改一個地方：

```cpp
enum class InputAction {
    // ... 現有的 actions

    // 添加新功能時補充
    Reload,
    UseItem,
    OpenInventory,
    // ...
};

// 然後在 loadDefaultMappings() 中綁定
inputManager.mapKey(SDL_SCANCODE_R, InputAction::Reload);
```

### 什麼時候才需要 String-based？

**極少數情況：**

1. **支援 MOD 創建全新玩法**
   - 例如：MOD 添加「飛行模式」但原遊戲沒有
   - 解決方案：提供 MOD API，讓 MOD 註冊新 Action

2. **完全數據驅動的遊戲**
   - 整個遊戲邏輯都從配置文件載入
   - 這種情況下整個引擎架構都會不同

3. **通用輸入工具/調試器**
   - 不知道會處理什麼遊戲
   - 需要支援任意 action

**對於 project-vapor 的 demo：完全不需要。**

### 如果將來真的需要怎麼辦？

#### 方案 1: 改用 String（最簡單）

只需修改一個檔案：

```cpp
// input_manager.hpp
// 之前
enum class InputAction { /* ... */ };

// 之後
using InputAction = std::string;
namespace InputActions {
    inline const std::string MoveForward = "move_forward";
    inline const std::string Jump = "jump";
    // ...
}
```

其他程式碼幾乎不用改！因為 API 相同：
```cpp
inputState.isPressed(InputAction::Jump);      // enum 版本
inputState.isPressed(InputActions::Jump);     // string 版本
inputState.isPressed("custom_action");        // 動態 string
```

#### 方案 2: Hybrid（保留 enum + 擴展）

```cpp
enum class InputAction : uint32_t {
    // Built-in actions (0-999)
    MoveForward = 0,
    Jump = 1,
    // ...

    // Reserved for dynamic actions (1000+)
    DynamicStart = 1000
};

class InputManager {
    InputAction registerDynamicAction(const std::string& name) {
        auto id = static_cast<InputAction>(m_nextDynamicID++);
        m_dynamicActionNames[id] = name;
        return id;
    }
private:
    uint32_t m_nextDynamicID = 1000;
    std::unordered_map<InputAction, std::string> m_dynamicActionNames;
};
```

### 結論：為什麼選擇 Enum

| 考量因素 | Enum | String |
|---------|------|--------|
| **類型安全** | ✅ 編譯時檢查 | ❌ 運行時才知道錯誤 |
| **效能** | ✅ 整數比較 | ❌ String 比較 |
| **開發體驗** | ✅ IDE 自動補全 | ❌ 需要記憶/查文檔 |
| **重新綁定按鍵** | ✅ 完全支援 | ✅ 完全支援 |
| **動態創建 Action** | ❌ 不支援 | ✅ 支援 |
| **適用場景** | ✅ 99% 的遊戲 | ⚠️ 極特殊需求 |

**建議：**
- ✅ Demo 和正式版都用 Enum
- ✅ 只有在真正需要動態 Action 時才考慮 String
- ✅ 如果將來需要，遷移成本很低（1-2 小時）

---

## 未來擴展詳細指南

以下是當前實作的擴展路徑，按優先級和複雜度排序。

---

### 1. ImGui 調試面板（推薦優先實作）

**難度：** ⭐ 簡單
**時間：** 30 分鐘
**價值：** 🔥🔥🔥 極高（開發效率提升）

#### 實作範例

在 `input_manager.hpp` 中添加：

```cpp
class InputManager {
public:
    // ... 現有方法

    #ifdef IMGUI_VERSION
    void drawImGuiDebugPanel();
    #endif
};
```

在 `input_manager.cpp` 中實作：

```cpp
#ifdef IMGUI_VERSION
#include "imgui.h"

void InputManager::drawImGuiDebugPanel() {
    if (ImGui::Begin("Input Manager Debug")) {
        // 顯示當前按下的 Actions
        ImGui::SeparatorText("Current Input State");

        ImGui::Text("Held Actions:");
        for (auto action : m_currentState.m_heldActions) {
            ImGui::BulletText("%s", actionToString(action).c_str());
        }

        if (!m_currentState.m_pressedActions.empty()) {
            ImGui::TextColored(ImVec4(0, 1, 0, 1), "Pressed This Frame:");
            for (auto action : m_currentState.m_pressedActions) {
                ImGui::BulletText("%s", actionToString(action).c_str());
            }
        }

        if (!m_currentState.m_releasedActions.empty()) {
            ImGui::TextColored(ImVec4(1, 0, 0, 1), "Released This Frame:");
            for (auto action : m_currentState.m_releasedActions) {
                ImGui::BulletText("%s", actionToString(action).c_str());
            }
        }

        // 顯示 Input Buffer
        ImGui::SeparatorText("Input Buffer (Recent Events)");
        ImGui::Text("Buffer Size: %zu / %zu", m_inputBuffer.size(), m_maxBufferSize);

        if (ImGui::BeginTable("InputBuffer", 2, ImGuiTableFlags_Borders)) {
            ImGui::TableSetupColumn("Time (ms ago)");
            ImGui::TableSetupColumn("Action");
            ImGui::TableHeadersRow();

            for (const auto& event : m_inputBuffer) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::Text("%llu", m_currentTime - event.timestamp);
                ImGui::TableNextColumn();
                ImGui::Text("%s", actionToString(event.action).c_str());
            }
            ImGui::EndTable();
        }

        // 顯示按鍵綁定
        ImGui::SeparatorText("Key Mappings");
        if (ImGui::BeginTable("KeyMappings", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY)) {
            ImGui::TableSetupColumn("Key");
            ImGui::TableSetupColumn("Action");
            ImGui::TableHeadersRow();

            for (const auto& [key, action] : m_keyToAction) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::Text("%s", SDL_GetScancodeName(key));
                ImGui::TableNextColumn();
                ImGui::Text("%s", actionToString(action).c_str());
            }
            ImGui::EndTable();
        }

        // 滑鼠狀態
        ImGui::SeparatorText("Mouse State");
        ImGui::Text("Position: (%.1f, %.1f)", m_mousePosition.x, m_mousePosition.y);
        ImGui::Text("Delta: (%.1f, %.1f)", m_mouseDelta.x, m_mouseDelta.y);
    }
    ImGui::End();
}

// 輔助函數：將 InputAction 轉成字串
static std::string actionToString(InputAction action) {
    switch (action) {
        case InputAction::MoveForward: return "MoveForward";
        case InputAction::MoveBackward: return "MoveBackward";
        case InputAction::StrafeLeft: return "StrafeLeft";
        case InputAction::StrafeRight: return "StrafeRight";
        case InputAction::MoveUp: return "MoveUp";
        case InputAction::MoveDown: return "MoveDown";
        case InputAction::LookUp: return "LookUp";
        case InputAction::LookDown: return "LookDown";
        case InputAction::LookLeft: return "LookLeft";
        case InputAction::LookRight: return "LookRight";
        case InputAction::RollLeft: return "RollLeft";
        case InputAction::RollRight: return "RollRight";
        case InputAction::SwitchToFlyCam: return "SwitchToFlyCam";
        case InputAction::SwitchToFollowCam: return "SwitchToFollowCam";
        case InputAction::Jump: return "Jump";
        case InputAction::Crouch: return "Crouch";
        case InputAction::Sprint: return "Sprint";
        case InputAction::Interact: return "Interact";
        case InputAction::Cancel: return "Cancel";
        default: return "Unknown";
    }
}
#endif
```

在 `main.cpp` 中使用：

```cpp
// 在 ImGui 渲染部分
#ifdef IMGUI_VERSION
    inputManager.drawImGuiDebugPanel();
#endif
```

**效果：**
- 即時查看哪些 action 被觸發
- 檢視 input buffer 歷史記錄
- 調試按鍵綁定問題
- 觀察滑鼠移動

---

### 2. 支援按鍵綁定序列化（玩家自訂）

**難度：** ⭐⭐ 中等
**時間：** 1-2 小時
**價值：** 🔥🔥 高（玩家體驗）

#### 實作範例

在 `input_manager.hpp` 中添加：

```cpp
#include <nlohmann/json.hpp>  // 或使用其他序列化庫

class InputManager {
public:
    // ... 現有方法

    // 序列化/反序列化按鍵綁定
    nlohmann::json serializeBindings() const;
    void deserializeBindings(const nlohmann::json& data);

    // 保存/載入到文件
    bool saveBindingsToFile(const std::string& filepath) const;
    bool loadBindingsFromFile(const std::string& filepath);

    // 獲取 action 綁定的所有按鍵（用於 UI 顯示）
    std::vector<SDL_Scancode> getKeysForAction(InputAction action) const;
};
```

在 `input_manager.cpp` 中實作：

```cpp
#include <fstream>
#include <nlohmann/json.hpp>

nlohmann::json InputManager::serializeBindings() const {
    nlohmann::json j;

    for (const auto& [key, action] : m_keyToAction) {
        std::string keyName = SDL_GetScancodeName(key);
        std::string actionName = actionToString(action);
        j["bindings"].push_back({
            {"key", keyName},
            {"scancode", static_cast<int>(key)},
            {"action", actionName}
        });
    }

    return j;
}

void InputManager::deserializeBindings(const nlohmann::json& j) {
    // 清除現有綁定
    clearMappings();

    if (j.contains("bindings")) {
        for (const auto& binding : j["bindings"]) {
            SDL_Scancode key = static_cast<SDL_Scancode>(binding["scancode"].get<int>());
            std::string actionName = binding["action"].get<std::string>();
            InputAction action = stringToAction(actionName);

            mapKey(key, action);
        }
    }
}

bool InputManager::saveBindingsToFile(const std::string& filepath) const {
    try {
        std::ofstream file(filepath);
        if (!file.is_open()) return false;

        auto j = serializeBindings();
        file << j.dump(4);  // 美化輸出，縮排 4 格
        return true;
    } catch (...) {
        return false;
    }
}

bool InputManager::loadBindingsFromFile(const std::string& filepath) {
    try {
        std::ifstream file(filepath);
        if (!file.is_open()) {
            // 文件不存在，載入預設綁定
            loadDefaultMappings();
            return false;
        }

        nlohmann::json j;
        file >> j;
        deserializeBindings(j);
        return true;
    } catch (...) {
        // 發生錯誤，載入預設綁定
        loadDefaultMappings();
        return false;
    }
}

std::vector<SDL_Scancode> InputManager::getKeysForAction(InputAction action) const {
    std::vector<SDL_Scancode> keys;
    for (const auto& [key, act] : m_keyToAction) {
        if (act == action) {
            keys.push_back(key);
        }
    }
    return keys;
}

// 輔助函數：字串轉 InputAction
static InputAction stringToAction(const std::string& str) {
    static const std::unordered_map<std::string, InputAction> map = {
        {"MoveForward", InputAction::MoveForward},
        {"MoveBackward", InputAction::MoveBackward},
        {"StrafeLeft", InputAction::StrafeLeft},
        {"StrafeRight", InputAction::StrafeRight},
        // ... 其他所有 actions
    };

    auto it = map.find(str);
    if (it != map.end()) {
        return it->second;
    }

    // 預設值
    return InputAction::Cancel;
}
```

**使用範例：**

```cpp
// 在遊戲啟動時載入玩家的自訂綁定
engineCore->getInputManager().loadBindingsFromFile("config/keybindings.json");

// 在設定選單中保存
engineCore->getInputManager().saveBindingsToFile("config/keybindings.json");

// 在設定 UI 中顯示
auto keys = inputManager.getKeysForAction(InputAction::Jump);
ImGui::Text("Jump: %s", SDL_GetScancodeName(keys[0]));
```

**生成的 JSON 格式：**

```json
{
    "bindings": [
        {
            "key": "W",
            "scancode": 26,
            "action": "MoveForward"
        },
        {
            "key": "Space",
            "scancode": 44,
            "action": "Jump"
        }
    ]
}
```

---

### 3. 手把（遊戲控制器）支援

**難度：** ⭐⭐⭐ 中高
**時間：** 2-3 小時
**價值：** 🔥🔥 中高（取決於目標平台）

#### 實作概要

擴展 `InputAction` 和事件處理：

```cpp
// input_manager.hpp
class InputManager {
public:
    // 添加手把按鈕映射
    void mapButton(SDL_GamepadButton button, InputAction action);
    void unmapButton(SDL_GamepadButton button);

    // 連接/斷開事件
    void onGamepadConnected(SDL_JoystickID id);
    void onGamepadDisconnected(SDL_JoystickID id);

private:
    std::unordered_map<SDL_GamepadButton, InputAction> m_buttonToAction;
    std::vector<SDL_Gamepad*> m_gamepads;
};
```

```cpp
// input_manager.cpp
void InputManager::processEvent(const SDL_Event& event) {
    switch (event.type) {
        // ... 現有的按鍵處理

        case SDL_EVENT_GAMEPAD_BUTTON_DOWN: {
            auto it = m_buttonToAction.find(
                static_cast<SDL_GamepadButton>(event.gbutton.button)
            );
            if (it != m_buttonToAction.end()) {
                InputAction action = it->second;
                if (m_currentState.m_heldActions.find(action) == m_currentState.m_heldActions.end()) {
                    m_currentState.m_heldActions.insert(action);
                    m_currentState.m_pressedActions.insert(action);
                    addToBuffer(action);
                }
            }
            break;
        }

        case SDL_EVENT_GAMEPAD_BUTTON_UP: {
            auto it = m_buttonToAction.find(
                static_cast<SDL_GamepadButton>(event.gbutton.button)
            );
            if (it != m_buttonToAction.end()) {
                InputAction action = it->second;
                if (m_currentState.m_heldActions.find(action) != m_currentState.m_heldActions.end()) {
                    m_currentState.m_heldActions.erase(action);
                    m_currentState.m_releasedActions.insert(action);
                }
            }
            break;
        }

        case SDL_EVENT_GAMEPAD_ADDED: {
            onGamepadConnected(event.gdevice.which);
            break;
        }

        case SDL_EVENT_GAMEPAD_REMOVED: {
            onGamepadDisconnected(event.gdevice.which);
            break;
        }
    }
}

void InputManager::loadDefaultMappings() {
    // ... 現有的鍵盤映射

    // 添加手把預設映射
    mapButton(SDL_GAMEPAD_BUTTON_SOUTH, InputAction::Jump);  // A/Cross
    mapButton(SDL_GAMEPAD_BUTTON_EAST, InputAction::Cancel);  // B/Circle
    mapButton(SDL_GAMEPAD_BUTTON_START, InputAction::Interact);
    // ...
}
```

---

### 4. Axis 支援（搖桿、滑鼠靈敏度）

**難度：** ⭐⭐⭐ 中高
**時間：** 2-4 小時
**價值：** 🔥🔥🔥 高（相機控制、載具）

#### 設計概要

添加 Axis 抽象：

```cpp
// input_manager.hpp
enum class InputAxis {
    MoveHorizontal,   // A/D 或搖桿左 X
    MoveVertical,     // W/S 或搖桿左 Y
    LookHorizontal,   // 滑鼠 X 或搖桿右 X
    LookVertical,     // 滑鼠 Y 或搖桿右 Y
    // ...
};

struct AxisMapping {
    InputAction negativeAction;  // 例如 MoveLeft
    InputAction positiveAction;  // 例如 MoveRight
    float deadzone = 0.1f;
    float sensitivity = 1.0f;
};

class InputManager {
public:
    // Axis 映射
    void mapAxis(InputAxis axis, const AxisMapping& mapping);

    // 獲取 axis 值 (-1.0 到 1.0)
    float getAxis(InputAxis axis) const;
    glm::vec2 getAxis2D(InputAxis horizontal, InputAxis vertical) const;

    // 設定靈敏度
    void setAxisSensitivity(InputAxis axis, float sensitivity);
    void setAxisDeadzone(InputAxis axis, float deadzone);

private:
    std::unordered_map<InputAxis, AxisMapping> m_axisMappings;
    std::unordered_map<InputAxis, float> m_axisValues;  // 當前 axis 值
};
```

**使用範例：**

```cpp
// 設定 axis
AxisMapping moveHorizontal;
moveHorizontal.negativeAction = InputAction::StrafeLeft;
moveHorizontal.positiveAction = InputAction::StrafeRight;
moveHorizontal.sensitivity = 1.0f;
inputManager.mapAxis(InputAxis::MoveHorizontal, moveHorizontal);

// 在相機更新中使用
float horizontal = inputManager.getAxis(InputAxis::LookHorizontal);
float vertical = inputManager.getAxis(InputAxis::LookVertical);
camera.rotate(horizontal * sensitivity * dt, vertical * sensitivity * dt);

// 或使用 2D vector
auto movement = inputManager.getAxis2D(InputAxis::MoveHorizontal, InputAxis::MoveVertical);
player.move(movement * speed * dt);
```

---

### 5. 輸入錄製/回放（測試用）

**難度：** ⭐⭐⭐⭐ 高
**時間：** 4-6 小時
**價值：** 🔥 中（自動化測試）

**概念：**
- 錄製一段遊戲輸入序列
- 回放以重現 bug 或測試行為
- 用於自動化測試、TAS（Tool-Assisted Speedrun）

**簡化實作：**

```cpp
class InputRecorder {
public:
    void startRecording();
    void stopRecording();
    void saveRecording(const std::string& filepath);

    void loadRecording(const std::string& filepath);
    void startPlayback();
    void stopPlayback();

    // 每幀調用
    void update(InputManager& inputManager, float deltaTime);

private:
    struct RecordedEvent {
        float timestamp;
        InputAction action;
        bool pressed;  // true = pressed, false = released
    };

    std::vector<RecordedEvent> m_recording;
    bool m_isRecording = false;
    bool m_isPlayingBack = false;
    float m_playbackTime = 0.0f;
    size_t m_playbackIndex = 0;
};
```

---

### 6. 連擊檢測系統

**難度：** ⭐⭐⭐ 中
**時間：** 2-3 小時
**價值：** 🔥🔥 中（格鬥遊戲、動作遊戲）

**概念：**
- 檢測特定按鍵序列（例如：上上下下左右左右BA）
- 時間窗口內完成

**簡化實作：**

```cpp
class ComboDetector {
public:
    struct Combo {
        std::vector<InputAction> sequence;
        float maxTimeWindow;  // 毫秒
        std::function<void()> callback;
    };

    void registerCombo(const Combo& combo);
    void update(const InputManager& inputManager);

private:
    std::vector<Combo> m_combos;
};

// 使用
ComboDetector comboDetector;
comboDetector.registerCombo({
    .sequence = {InputAction::Jump, InputAction::Jump, InputAction::Crouch},
    .maxTimeWindow = 500.0f,  // 500ms
    .callback = []() { player.performSpecialMove(); }
});
```

---

## 總結：擴展路徑建議

### 優先級排序

1. **立即實作（Demo 必要）：**
   - ✅ 已完成：基本 InputManager

2. **短期（1 週內）：**
   - 🔥 **ImGui 調試面板** - 極大提升開發效率
   - 📝 按鍵綁定序列化 - 玩家體驗基本需求

3. **中期（1 個月內）：**
   - 🎮 手把支援 - 如果目標支援主機/手把玩家
   - 🎯 Axis 支援 - 更好的相機控制

4. **長期（有需要再做）：**
   - 🎬 輸入錄製/回放 - 自動化測試
   - 👊 連擊檢測 - 特定遊戲類型需要

### 最小可行產品（MVP）

**Demo 階段只需要：**
- ✅ 當前的 enum-based InputManager
- ✅ ImGui 調試面板（30 分鐘工作量，價值極高）

**其他都可以等到實際需要時再加。**
