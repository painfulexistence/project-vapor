# EnTT ECS 重構指南

## 概述

Project Vapor 已經整合了 [EnTT](https://github.com/skypjack/entt) 這個現代化的 Entity-Component-System (ECS) 函式庫，讓 gameplay code 更容易編寫。

## 為什麼使用 ECS？

### 傳統場景圖的問題

```cpp
// 舊方式：Node 包含所有東西，難以擴展
auto node = scene->createNode("Player");
node->setPosition(glm::vec3(0, 1, 0));
node->body = physics->createBody(...);
scene->addMeshToNode(node, mesh);
// 如果想加新功能（如 AI、Health），需要修改 Node 類別
```

### ECS 的優勢

```cpp
// 新方式：組合式設計，易於擴展
auto player = scene.createEntity("Player");
scene.addTransform(player, glm::vec3(0, 1, 0));
scene.addRigidBody(player, body);
scene.addMeshRenderer(player, mesh);

// 輕鬆添加自定義組件
registry.emplace<Health>(player, 100.0f);
registry.emplace<PlayerController>(player);
registry.emplace<Inventory>(player);
```

**主要優勢：**
1. ✅ **組合優於繼承** - 靈活組合功能
2. ✅ **數據導向設計** - 更好的緩存性能
3. ✅ **易於編寫遊戲邏輯** - 清晰的組件和系統分離
4. ✅ **可擴展性** - 高效處理大量實體
5. ✅ **並行友好** - 系統可以並行運行

## 核心概念

### Entity（實體）
實體只是一個 ID，本身不包含任何數據。

```cpp
entt::entity player = scene.createEntity("Player");
```

### Component（組件）
組件是純數據結構，定義實體的屬性。

```cpp
// 內建組件
struct Transform {
    glm::vec3 position;
    glm::quat rotation;
    glm::vec3 scale;
    // ...
};

struct RigidBody {
    BodyHandle handle;
    float mass;
    bool isKinematic;
};

// 自定義組件
struct Health {
    float current;
    float max;
};

struct PlayerController {
    float moveSpeed;
    float jumpForce;
};
```

### System（系統）
系統包含邏輯，處理具有特定組件的實體。

```cpp
// Transform 系統 - 更新世界變換矩陣
TransformSystem::update(registry);

// Physics 系統 - 同步物理和變換
PhysicsSystem physicsSystem(physics);
physicsSystem.update(registry, deltaTime);

// 自定義系統
void updateHealth(entt::registry& registry, float dt) {
    auto view = registry.view<Health>();
    for (auto entity : view) {
        auto& health = view.get<Health>(entity);
        // 處理生命值邏輯
    }
}
```

## 快速開始

### 1. 創建場景

```cpp
#include "Vapor/scene_ecs.hpp"

ECSScene scene("MyGame");
```

### 2. 創建實體並添加組件

```cpp
// 創建玩家
auto player = scene.createEntity("Player");

// 添加變換組件
scene.addTransform(player,
                  glm::vec3(0, 1, 0),      // position
                  glm::quat(1, 0, 0, 0),   // rotation
                  glm::vec3(1, 1, 1));     // scale

// 添加網格渲染器
auto material = std::make_shared<Material>();
auto mesh = MeshBuilder::buildCube(1.0f, material);
scene.addMeshRenderer(player, mesh);

// 添加物理體
auto body = physics->createBoxBody(
    glm::vec3(0.5f, 0.5f, 0.5f),
    glm::vec3(0, 1, 0),
    glm::quat(1, 0, 0, 0),
    BodyMotionType::Dynamic
);
physics->addBody(body, true);
scene.addRigidBody(player, body, 1.0f);
```

### 3. 設置層級關係

```cpp
auto parent = scene.createEntity("Parent");
scene.addTransform(parent);

auto child = scene.createEntity("Child");
scene.addTransform(child, glm::vec3(2, 0, 0));

// 建立父子關係
scene.setParent(child, parent);
```

### 4. 更新場景

```cpp
// 在遊戲循環中
void update(float deltaTime) {
    // 更新變換（處理層級傳播）
    scene.updateTransforms();

    // 更新物理
    PhysicsSystem physicsSystem(physics);
    physicsSystem.update(scene.getRegistry(), deltaTime);

    // 渲染
    renderer->draw(&scene, camera);
}
```

## 進階用法

### 直接訪問 Registry

對於複雜的遊戲邏輯，直接使用 EnTT registry：

```cpp
auto& registry = scene.getRegistry();

// 遍歷所有具有特定組件的實體
auto view = registry.view<Transform, Health>();
for (auto entity : view) {
    auto& transform = view.get<Transform>(entity);
    auto& health = view.get<Health>(entity);

    // 你的遊戲邏輯
    if (health.current <= 0) {
        // 播放死亡動畫
        // 移除實體等
    }
}
```

### 添加自定義組件

```cpp
// 1. 定義組件結構
struct EnemyAI {
    float detectionRadius = 10.0f;
    float attackRange = 2.0f;
    entt::entity target = entt::null;
};

// 2. 添加到實體
auto enemy = scene.createEntity("Enemy");
registry.emplace<EnemyAI>(enemy);

// 3. 創建系統處理它
void updateEnemyAI(entt::registry& registry, float dt) {
    auto enemies = registry.view<Transform, EnemyAI>();

    for (auto entity : enemies) {
        auto& transform = enemies.get<Transform>(entity);
        auto& ai = enemies.get<EnemyAI>(entity);

        // AI 邏輯
        if (ai.target != entt::null) {
            auto& targetTransform = registry.get<Transform>(ai.target);
            glm::vec3 direction = targetTransform.position - transform.position;
            // 移動向目標...
        }
    }
}
```

### 使用標籤組件進行過濾

```cpp
// 定義標籤
struct Player {};
struct Enemy {};
struct Active {};

// 添加標籤
registry.emplace<Player>(playerEntity);
registry.emplace<Enemy>(enemyEntity);
registry.emplace<Active>(playerEntity);

// 只處理活躍的敵人
auto activeEnemies = registry.view<Enemy, Active, Transform>();
for (auto entity : activeEnemies) {
    // 處理活躍敵人
}
```

### 組件事件監聽

```cpp
// 監聽組件添加/移除事件
registry.on_construct<Health>().connect<&onHealthAdded>();
registry.on_destroy<Health>().connect<&onHealthRemoved>();

void onHealthAdded(entt::registry& registry, entt::entity entity) {
    fmt::print("Health component added to entity\n");
}
```

## 性能優化建議

### 1. 使用 view 而不是單獨獲取組件

```cpp
// ❌ 慢
for (auto entity : entities) {
    auto& transform = registry.get<Transform>(entity);
    auto& velocity = registry.get<Velocity>(entity);
}

// ✅ 快
auto view = registry.view<Transform, Velocity>();
for (auto entity : view) {
    auto [transform, velocity] = view.get<Transform, Velocity>(entity);
}
```

### 2. 使用 group 進行更好的緩存性能

```cpp
// 對於頻繁一起訪問的組件，使用 group
auto group = registry.group<Transform>(entt::get<Velocity, RigidBody>);
for (auto entity : group) {
    auto [transform, velocity, body] = group.get<Transform, Velocity, RigidBody>(entity);
}
```

### 3. 批量創建實體

```cpp
// 創建大量實體時
std::vector<entt::entity> entities(1000);
registry.create(entities.begin(), entities.end());

for (auto entity : entities) {
    registry.emplace<Transform>(entity);
}
```

## 完整遊戲示例

```cpp
// 定義遊戲組件
struct Health { float value = 100.0f; };
struct Damage { float value = 10.0f; };
struct Velocity { glm::vec3 value{0, 0, 0}; };

// 創建遊戲系統
void movementSystem(entt::registry& registry, float dt) {
    auto view = registry.view<Transform, Velocity>();
    for (auto entity : view) {
        auto& transform = view.get<Transform>(entity);
        auto& velocity = view.get<Velocity>(entity);
        transform.position += velocity.value * dt;
    }
}

void combatSystem(entt::registry& registry) {
    auto players = registry.view<Transform, Health>(entt::exclude<Enemy>);
    auto enemies = registry.view<Transform, Damage, Enemy>();

    for (auto player : players) {
        auto& playerPos = players.get<Transform>(player).position;
        auto& health = players.get<Health>(player);

        for (auto enemy : enemies) {
            auto& enemyPos = enemies.get<Transform>(enemy).position;
            auto& damage = enemies.get<Damage>(enemy);

            float distance = glm::distance(playerPos, enemyPos);
            if (distance < 2.0f) {
                health.value -= damage.value;
            }
        }
    }
}

// 遊戲循環
void gameLoop(ECSScene& scene, float deltaTime) {
    auto& registry = scene.getRegistry();

    movementSystem(registry, deltaTime);
    combatSystem(registry);
    scene.updateTransforms();

    // 清理死亡實體
    auto deadEntities = registry.view<Health>();
    for (auto entity : deadEntities) {
        if (deadEntities.get<Health>(entity).value <= 0) {
            scene.destroyEntity(entity);
        }
    }
}
```

## 遷移指南

### 從舊的 Node 系統遷移

**舊代碼：**
```cpp
auto node = scene->createNode("Enemy");
node->setPosition(glm::vec3(5, 0, 0));
node->body = physics->createBody(...);
scene->addMeshToNode(node, mesh);
```

**新代碼：**
```cpp
auto enemy = scene.createEntity("Enemy");
scene.addTransform(enemy, glm::vec3(5, 0, 0));
scene.addRigidBody(enemy, physics->createBody(...));
scene.addMeshRenderer(enemy, mesh);
```

## 參考資料

- [EnTT 官方文檔](https://github.com/skypjack/entt)
- [示例代碼](../examples/ecs_example.cpp)
- [ECS 模式介紹](https://en.wikipedia.org/wiki/Entity_component_system)

## 總結

EnTT ECS 架構讓 gameplay code 變得：
- 🎮 **更容易編寫** - 清晰的組件和系統分離
- 🚀 **性能更好** - 數據導向設計，緩存友好
- 🔧 **更易維護** - 組合而非繼承
- 📈 **可擴展** - 輕鬆添加新功能

開始使用 ECS，讓你的遊戲開發更加愉快！
