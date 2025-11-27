# ActionManager - 时间驱动的行为序列系统

ActionManager 是一个强大的时间驱动行为管理系统，用于处理延迟操作、动画序列和基于时间的游戏逻辑。

## 架构概览

系统由以下核心组件组成：

### 核心类

1. **Timer** - 简单的计时器工具类
2. **Action** - 所有 action 的基类
3. **ActionManager** - 管理和更新所有活动的 action

### 内置 Action 类型

- **DelayAction** - 延迟等待
- **CallbackAction** - 立即执行回调
- **TimedCallbackAction** - 延迟执行回调
- **UpdateAction** - 带进度的更新回调（运行指定时长）
- **UpdateForeverAction** - 永久运行的更新回调
- **TimelineAction** - 顺序执行多个 action
- **ParallelAction** - 并行执行多个 action
- **RepeatAction** - 重复执行 action

### 缓动函数 (Easing Functions)

在 `Vapor::Easing` 命名空间中提供了多种缓动函数：
- `Linear` - 线性
- `InQuad`, `OutQuad`, `InOutQuad` - 二次方缓动
- `InCubic`, `OutCubic`, `InOutCubic` - 三次方缓动
- `OutBack` - 回弹效果

## 使用方法

### 1. 基本用法

```cpp
#include "Vapor/action_manager.hpp"
using namespace Vapor;

// 在 EngineCore 中获取 ActionManager
auto& actionManager = EngineCore::Get()->getActionManager();

// 在游戏循环中更新
void update(float deltaTime) {
    EngineCore::Get()->update(deltaTime);  // 自动更新 ActionManager
}
```

### 2. 延迟执行

```cpp
// 等待 2 秒
auto delay = std::make_shared<DelayAction>(2.0f);
actionManager.start(delay);

// 2 秒后执行回调
auto timedCallback = std::make_shared<TimedCallbackAction>(2.0f, []() {
    std::cout << "2 seconds passed!" << std::endl;
});
actionManager.start(timedCallback);
```

### 3. 创建动作序列

```cpp
// 创建一个时间线（顺序执行）
auto timeline = std::make_shared<TimelineAction>();
timeline->add(std::make_shared<CallbackAction>([]() {
    std::cout << "Start" << std::endl;
}));
timeline->add(std::make_shared<DelayAction>(1.0f));  // 等待 1 秒
timeline->add(std::make_shared<CallbackAction>([]() {
    std::cout << "After 1 second" << std::endl;
}));
timeline->add(std::make_shared<DelayAction>(0.5f));  // 再等待 0.5 秒
timeline->add(std::make_shared<CallbackAction>([]() {
    std::cout << "Complete!" << std::endl;
}));

actionManager.start(timeline, "my_sequence");
```

### 4. 并行执行

```cpp
// 多个 action 同时执行
auto parallel = std::make_shared<ParallelAction>();
parallel->add(std::make_shared<TimedCallbackAction>(1.0f, []() {
    std::cout << "Action 1 done" << std::endl;
}));
parallel->add(std::make_shared<TimedCallbackAction>(2.0f, []() {
    std::cout << "Action 2 done" << std::endl;
}));
// 并行 action 在所有子 action 完成后才会结束

actionManager.start(parallel);
```

### 5. 重复执行

```cpp
// 重复 3 次
auto repeat3 = std::make_shared<RepeatAction>(
    std::make_shared<TimedCallbackAction>(0.5f, []() {
        std::cout << "Tick!" << std::endl;
    }),
    3  // 重复次数
);
actionManager.start(repeat3);

// 无限重复 (-1)
auto repeatForever = std::make_shared<RepeatAction>(
    std::make_shared<UpdateAction>(1.0f, [](float dt, float progress) {
        // 每秒重复一次
    }),
    -1  // 无限重复
);
actionManager.start(repeatForever, "infinite_loop");
```

### 6. 使用标签管理

```cpp
// 使用标签启动多个 action
actionManager.start(idleAnimation, "player_idle");
actionManager.start(idleSound, "player_idle");

// 停止所有带有该标签的 action
actionManager.stopByTag("player_idle");

// 检查是否有带标签的 action 在运行
if (actionManager.hasTag("player_idle")) {
    // ...
}
```

### 7. 更新回调

```cpp
// 带进度的更新回调
auto updateAction = std::make_shared<UpdateAction>(3.0f,
    [](float dt, float progress) {
        // dt: 每帧的增量时间
        // progress: 0.0 到 1.0 的进度值
        std::cout << "Progress: " << (progress * 100) << "%" << std::endl;
    }
);
actionManager.start(updateAction);

// 永久运行的更新回调
auto foreverAction = std::make_shared<UpdateForeverAction>(
    [](float dt) {
        // 每帧都会调用
    }
);
actionManager.start(foreverAction, "continuous_update");
```

## 实际应用示例

### 示例 1: 角色受击反馈

```cpp
void onPlayerHit() {
    auto hitSequence = std::make_shared<TimelineAction>();

    // 立即播放受击音效
    hitSequence->add(std::make_shared<CallbackAction>([this]() {
        playSound("hit.wav");
    }));

    // 短暂停顿（hit pause）
    hitSequence->add(std::make_shared<DelayAction>(0.1f));

    // 开始闪烁
    hitSequence->add(std::make_shared<CallbackAction>([this]() {
        startFlashing();
    }));

    // 闪烁持续 0.5 秒
    hitSequence->add(std::make_shared<DelayAction>(0.5f));

    // 停止闪烁
    hitSequence->add(std::make_shared<CallbackAction>([this]() {
        stopFlashing();
    }));

    actionManager.start(hitSequence, "player_hit_feedback");
}
```

### 示例 2: 物品收集效果

```cpp
void onItemCollected(glm::vec3 itemPosition) {
    auto collectSequence = std::make_shared<ParallelAction>();

    // 移动到玩家位置的动画（需要自定义 Action）
    // collectSequence->add(std::make_shared<MoveToAction>(playerPosition, 0.5f));

    // 同时播放音效
    collectSequence->add(std::make_shared<CallbackAction>([]() {
        playSound("collect.wav");
    }));

    // 延迟后增加分数
    collectSequence->add(std::make_shared<TimedCallbackAction>(0.5f, [this]() {
        addScore(100);
    }));

    actionManager.start(collectSequence);
}
```

### 示例 3: 周期性检查

```cpp
void startEnemyAI() {
    // 每 0.5 秒检查一次玩家距离
    auto aiUpdate = std::make_shared<RepeatAction>(
        std::make_shared<TimedCallbackAction>(0.5f, [this]() {
            float distance = getDistanceToPlayer();
            if (distance < attackRange) {
                attack();
            }
        }),
        -1  // 无限重复
    );

    actionManager.start(aiUpdate, "enemy_ai");
}

void onEnemyDeath() {
    // 停止 AI 更新
    actionManager.stopByTag("enemy_ai");
}
```

## 自定义 Action

你可以继承 `Action` 类来创建自定义 action：

```cpp
class FadeAction : public Action {
public:
    FadeAction(float duration, float targetAlpha)
        : m_timer(duration), m_targetAlpha(targetAlpha) {}

    void onStart() override {
        m_timer.reset();
        // 保存起始 alpha 值
        m_startAlpha = getCurrentAlpha();
    }

    void update(float dt) override {
        if (m_timer.update(dt)) {
            m_finished = true;
            setAlpha(m_targetAlpha);
        } else {
            float progress = m_timer.getProgress();
            float currentAlpha = m_startAlpha +
                (m_targetAlpha - m_startAlpha) * progress;
            setAlpha(currentAlpha);
        }
    }

private:
    Timer m_timer;
    float m_startAlpha;
    float m_targetAlpha;
};
```

## 集成到引擎

ActionManager 已经集成到 `EngineCore` 中：

```cpp
// 初始化引擎时自动初始化 ActionManager
EngineCore engine;
engine.init();

// 获取 ActionManager
auto& actionManager = engine.getActionManager();

// 在游戏循环中自动更新
engine.update(deltaTime);  // 内部会调用 actionManager.update(deltaTime)
```

## 性能建议

1. **避免频繁创建/销毁 action** - 对于高频操作，考虑使用对象池
2. **合理使用标签** - 标签便于批量管理 action
3. **及时清理** - 状态切换时使用 `stopByTag()` 清理相关 action
4. **使用 UpdateForeverAction 谨慎** - 确保在不需要时停止它们

## API 参考

### ActionManager

```cpp
// 启动一个 action（可选标签）
std::shared_ptr<Action> start(std::shared_ptr<Action> action, const std::string& tag = "");

// 停止特定 action
void stop(const std::shared_ptr<Action>& action);

// 停止所有带指定标签的 action
void stopByTag(const std::string& tag);

// 停止所有 action
void stopAll();

// 检查是否有带标签的 action
bool hasTag(const std::string& tag) const;

// 获取所有带标签的 action
std::vector<std::shared_ptr<Action>> getActionsByTag(const std::string& tag) const;

// 更新所有 action
void update(float dt);
```

### Timer

```cpp
Timer(float duration);
void reset(float duration = -1.0f);
bool update(float dt);
bool isComplete() const;
float getProgress() const;  // 返回 0.0 到 1.0
```

## 与 Python 版本的对应关系

本 C++ 实现基于 Python 版本的 ActionManager，主要差异：

| Python | C++ |
|--------|-----|
| `action_manager.start(action, owner=obj)` | `actionManager.start(action)` (无需 owner) |
| `class MyAction(Action):` | `class MyAction : public Action` |
| `self.finished = True` | `m_finished = true;` 或 `finish()` |
| `timer.get_progress()` | `timer.getProgress()` |

## 示例代码

完整示例请参考：`Vapor/examples/action_manager_example.cpp`

编译示例（如果使用独立编译）：
```bash
g++ -std=c++20 -I./Vapor/include Vapor/examples/action_manager_example.cpp \
    Vapor/src/action_manager.cpp -o action_manager_example
./action_manager_example
```
