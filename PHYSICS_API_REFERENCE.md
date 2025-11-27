# Jolt Physics API Reference

完整的 Vapor 引擎物理系統 API 參考文檔

---

## 目錄

1. [基礎物理系統](#基礎物理系統)
2. [剛體創建](#剛體創建)
3. [剛體控制](#剛體控制)
4. [物理屬性](#物理屬性)
5. [碰撞檢測](#碰撞檢測)
6. [角色控制器](#角色控制器)
7. [車輛控制器](#車輛控制器)
8. [流體物理](#流體物理)
9. [物理事件](#物理事件)
10. [Gameplay 架構](#gameplay-架構)

---

## 基礎物理系統

### 初始化與管理

```cpp
#include "Vapor/physics_3d.hpp"

auto physics = std::make_unique<Physics3D>();

// 初始化（需要 TaskScheduler）
physics->init(engineCore->getTaskScheduler());

// 設置重力
physics->setGravity(glm::vec3(0.0f, -9.81f, 0.0f));

// 獲取當前重力
glm::vec3 gravity = physics->getGravity();

// 每幀更新（通常在主循環）
physics->process(scene, deltaTime);

// 清理
physics->deinit();
```

---

## 剛體創建

### 基本形狀

```cpp
// 盒子
BodyHandle box = physics->createBoxBody(
    glm::vec3(0.5f, 0.5f, 0.5f),  // 半尺寸 (half extents)
    glm::vec3(0.0f, 5.0f, 0.0f),  // 位置
    glm::identity<glm::quat>(),    // 旋轉
    BodyMotionType::Dynamic        // 運動類型
);

// 球體
BodyHandle sphere = physics->createSphereBody(
    0.5f,                          // 半徑
    glm::vec3(0.0f, 10.0f, 0.0f),
    glm::identity<glm::quat>(),
    BodyMotionType::Dynamic
);

// 膠囊體
BodyHandle capsule = physics->createCapsuleBody(
    0.9f,                          // 半高（不含半球）
    0.3f,                          // 半徑
    glm::vec3(0.0f, 2.0f, 0.0f),
    glm::identity<glm::quat>(),
    BodyMotionType::Dynamic
);

// 圓柱體
BodyHandle cylinder = physics->createCylinderBody(
    1.0f,                          // 半高
    0.5f,                          // 半徑
    glm::vec3(0.0f, 3.0f, 0.0f),
    glm::identity<glm::quat>(),
    BodyMotionType::Dynamic
);
```

### 複雜形狀

```cpp
// 網格碰撞體（靜態物體用）
std::vector<glm::vec3> vertices = { /* ... */ };
std::vector<Uint32> indices = { /* ... */ };

BodyHandle mesh = physics->createMeshBody(
    vertices,
    indices,
    glm::vec3(0.0f, 0.0f, 0.0f),
    glm::identity<glm::quat>(),
    BodyMotionType::Static  // 通常只用於靜態物體
);

// 凸包碰撞體（動態物體用）
std::vector<glm::vec3> points = {
    glm::vec3(-1, 0, 0),
    glm::vec3(1, 0, 0),
    glm::vec3(0, 2, 0),
    /* ... */
};

BodyHandle convex = physics->createConvexHullBody(
    points,
    glm::vec3(0.0f, 5.0f, 0.0f),
    glm::identity<glm::quat>(),
    BodyMotionType::Dynamic
);
```

### 運動類型

```cpp
enum class BodyMotionType {
    Static,    // 靜態：不移動，無限質量（地面、牆壁）
    Dynamic,   // 動態：完全受物理影響（可撿拾物體）
    Kinematic  // 運動學：可手動控制位置，不受力影響（電梯、平台）
};
```

### 添加到場景

```cpp
// 創建剛體後必須添加
physics->addBody(body, true);  // true = 立即激活

// 與 Node 關聯
node->body = body;
```

---

## 剛體控制

### 運動狀態

```cpp
// 改變運動類型
physics->setMotionType(body, BodyMotionType::Kinematic);
BodyMotionType type = physics->getMotionType(body);

// 激活/休眠
physics->activateBody(body);
physics->deactivateBody(body);
```

### 位置與旋轉

```cpp
// 設置位置
physics->setPosition(body, glm::vec3(10.0f, 5.0f, 0.0f));

// 設置旋轉（四元數）
glm::quat rotation = glm::angleAxis(glm::radians(45.0f), glm::vec3(0, 1, 0));
physics->setRotation(body, rotation);

// 獲取位置
glm::vec3 pos = physics->getPosition(body);

// 獲取旋轉
glm::quat rot = physics->getRotation(body);
```

### 速度控制

```cpp
// 線性速度（m/s）
physics->setLinearVelocity(body, glm::vec3(5.0f, 0.0f, 0.0f));
glm::vec3 vel = physics->getLinearVelocity(body);

// 角速度（rad/s）
physics->setAngularVelocity(body, glm::vec3(0.0f, 3.14f, 0.0f));
glm::vec3 angVel = physics->getAngularVelocity(body);
```

### 施加力與衝量

```cpp
// 在特定點施加力（持續作用）
physics->applyForce(
    body,
    glm::vec3(100.0f, 0.0f, 0.0f),  // 力向量（N）
    glm::vec3(0.0f, 1.0f, 0.0f)     // 相對位置
);

// 在質心施加力
physics->applyCentralForce(body, glm::vec3(100.0f, 0.0f, 0.0f));

// 施加扭矩
physics->applyTorque(body, glm::vec3(0.0f, 50.0f, 0.0f));

// 施加衝量（瞬間改變速度）
physics->applyImpulse(
    body,
    glm::vec3(500.0f, 0.0f, 0.0f),  // 衝量（N·s）
    glm::vec3(0.0f, 1.0f, 0.0f)
);

// 在質心施加衝量（常用於丟東西）
physics->applyCentralImpulse(body, glm::vec3(500.0f, 200.0f, 0.0f));

// 施加扭矩衝量
physics->applyTorqueImpulse(body, glm::vec3(0.0f, 100.0f, 0.0f));
```

---

## 物理屬性

```cpp
// 質量（kg）
physics->setMass(body, 10.0f);
float mass = physics->getMass(body);

// 彈性係數（0 = 不彈，1 = 完全彈性）
physics->setRestitution(body, 0.8f);
float restitution = physics->getRestitution(body);

// 摩擦係數（0 = 無摩擦，1 = 高摩擦）
physics->setFriction(body, 0.5f);
float friction = physics->getFriction(body);

// 重力係數（0 = 無重力，1 = 正常重力，負數 = 反重力）
physics->setGravityFactor(body, 0.0f);  // 漂浮
physics->setGravityFactor(body, -1.0f); // 往上飛
float gravityFactor = physics->getGravityFactor(body);

// 線性阻尼（空氣阻力）
physics->setLinearDamping(body, 0.05f);
float linearDamping = physics->getLinearDamping(body);

// 角阻尼（旋轉阻力）
physics->setAngularDamping(body, 0.05f);
float angularDamping = physics->getAngularDamping(body);
```

---

## 碰撞檢測

### Raycast（射線檢測）

```cpp
#include "Vapor/physics_3d.hpp"

RaycastHit hit;
glm::vec3 rayStart = camera.position;
glm::vec3 rayEnd = rayStart + camera.getForward() * 100.0f;

if (physics->raycast(rayStart, rayEnd, hit)) {
    Node* hitNode = hit.node;           // 碰撞的物體
    glm::vec3 hitPoint = hit.point;     // 碰撞點世界座標
    glm::vec3 hitNormal = hit.normal;   // 表面法線
    float distance = hit.distance;      // 距離起點的距離

    fmt::print("Hit: {} at distance {:.2f}\n", hitNode->name, distance);
}
```

### Trigger（觸發器）

觸發器是感應區域，不產生物理碰撞，只檢測進入/離開事件。

```cpp
// 創建盒形觸發器
TriggerHandle trigger = physics->createBoxTrigger(
    glm::vec3(2.0f, 2.0f, 2.0f),  // 半尺寸
    glm::vec3(0.0f, 1.0f, 0.0f),  // 位置
    glm::identity<glm::quat>()     // 旋轉（可選）
);

// 創建球形觸發器
TriggerHandle sphereTrigger = physics->createSphereTrigger(
    3.0f,                          // 半徑
    glm::vec3(10.0f, 0.0f, 0.0f)
);

// 創建膠囊形觸發器
TriggerHandle capsuleTrigger = physics->createCapsuleTrigger(
    1.0f,                          // 半高
    0.5f,                          // 半徑
    glm::vec3(0.0f, 1.0f, 5.0f)
);

// 與 Node 關聯
node->trigger = trigger;
```

### Overlap Test（重疊測試）

檢測某區域內的所有物體。

```cpp
// 球形重疊測試
OverlapResult result = physics->overlapSphere(
    glm::vec3(0.0f, 0.0f, 0.0f),  // 中心
    5.0f                           // 半徑
);

// 處理結果
for (Node* node : result.nodes) {
    fmt::print("Found object: {}\n", node->name);
}

for (BodyHandle body : result.bodies) {
    // 直接操作 body
    physics->applyCentralImpulse(body, glm::vec3(0, 500, 0));
}

// 盒形重疊測試
OverlapResult boxResult = physics->overlapBox(
    glm::vec3(0.0f, 0.0f, 0.0f),   // 中心
    glm::vec3(5.0f, 5.0f, 5.0f),   // 半尺寸
    glm::identity<glm::quat>()      // 旋轉
);

// 膠囊形重疊測試
OverlapResult capsuleResult = physics->overlapCapsule(
    glm::vec3(0.0f, 0.0f, 0.0f),   // 中心
    1.0f,                           // 半高
    0.5f,                           // 半徑
    glm::identity<glm::quat>()
);
```

---

## 角色控制器

基於膠囊體的角色控制器，適用於第一人稱/第三人稱角色。

### 創建與配置

```cpp
#include "Vapor/character_controller.hpp"

// 配置
CharacterControllerSettings settings {
    .height = 1.8f,                      // 角色高度（米）
    .radius = 0.3f,                      // 膠囊半徑
    .mass = 70.0f,                       // 質量（kg）
    .maxSlopeAngle = 45.0f,              // 最大爬坡角度（度）
    .maxStrength = 100.0f,               // 推開物體的力
    .characterPadding = 0.02f,           // 碰撞填充
    .penetrationRecoverySpeed = 1.0f,    // 穿透恢復速度
    .predictiveContactDistance = 0.1f    // 預測接觸距離
};

// 附加到 Node
node->attachCharacterController(physics.get(), settings);

// 獲取控制器
CharacterController* character = node->getCharacterController();
```

### 控制方法

```cpp
// 移動（相對速度）
glm::vec3 moveDirection(1.0f, 0.0f, 0.0f);  // 向右
character->move(moveDirection * 5.0f, deltaTime);

// 跳躍（只在地面時生效）
if (character->isOnGround()) {
    character->jump(5.0f);  // 跳躍速度（m/s）
}

// 傳送到指定位置
character->warp(glm::vec3(10.0f, 0.0f, 0.0f));

// 設置線性速度
character->setLinearVelocity(glm::vec3(0.0f, 0.0f, 0.0f));

// 設置最大速度
character->setMaxSpeed(10.0f);

// 設置重力（覆蓋全局重力）
character->setGravity(glm::vec3(0.0f, -15.0f, 0.0f));
```

### 狀態查詢

```cpp
// 是否在地面
if (character->isOnGround()) {
    // 可以跳躍
}

// 是否在陡峭表面滑動
if (character->isSliding()) {
    // 正在滑下斜坡
}

// 獲取位置
glm::vec3 pos = character->getPosition();

// 獲取速度
glm::vec3 vel = character->getVelocity();

// 獲取地面法線
glm::vec3 groundNormal = character->getGroundNormal();
```

### 典型使用範例

```cpp
// WASD 移動 + 空格跳躍
void updateCharacter(CharacterController* character, float deltaTime) {
    glm::vec3 moveDir(0.0f);

    if (keyPressed(W)) moveDir.z -= 1.0f;
    if (keyPressed(S)) moveDir.z += 1.0f;
    if (keyPressed(A)) moveDir.x -= 1.0f;
    if (keyPressed(D)) moveDir.x += 1.0f;

    if (glm::length(moveDir) > 0.0f) {
        moveDir = glm::normalize(moveDir) * 5.0f;  // 5 m/s
        character->move(moveDir, deltaTime);
    }

    if (keyPressed(SPACE) && character->isOnGround()) {
        character->jump(5.0f);
    }
}
```

---

## 車輛控制器

基於 Jolt 的 WheeledVehicleController，支援懸吊、引擎、變速箱、差速器。

### 創建與配置

```cpp
#include "Vapor/vehicle_controller.hpp"

// 使用預設模板（轎車）
VehicleSettings sedanSettings = VehicleSettings::createSedan();

// 使用預設模板（卡車）
VehicleSettings truckSettings = VehicleSettings::createTruck();

// 自定義配置
VehicleSettings customSettings {
    .mass = 2000.0f,
    .dimensions = glm::vec3(1.0f, 1.0f, 2.5f),  // 半尺寸
    .maxSteeringAngle = 0.6f,                    // 弧度
    .maxEngineTorque = 800.0f,
    .maxBrakeTorque = 2000.0f,
    .wheels = {
        // 前左輪
        WheelSettings {
            .position = glm::vec3(-0.9f, -0.5f, 1.5f),
            .wheelRadius = 0.35f,
            .wheelWidth = 0.25f,
            /* ... */
        },
        // 其他輪子 ...
    }
};

// 附加到 Node
node->attachVehicleController(physics.get(), sedanSettings);

// 獲取控制器
VehicleController* vehicle = node->getVehicleController();
```

### 控制方法

```cpp
// 油門/剎車（-1.0 到 1.0）
vehicle->setThrottle(1.0f);   // 全油門
vehicle->setThrottle(-0.5f);  // 倒車
vehicle->setThrottle(0.0f);   // 無輸入

// 轉向（-1.0 左，1.0 右）
vehicle->setSteering(0.5f);   // 半右轉

// 剎車（0.0 到 1.0）
vehicle->setBrake(1.0f);      // 全力剎車

// 手剎車（布林值）
vehicle->setHandbrake(true);
```

### 狀態查詢

```cpp
// 速度
float speed = vehicle->getSpeed();        // m/s
float speedKmh = vehicle->getSpeedKmh();  // km/h

// 位置與旋轉
glm::vec3 pos = vehicle->getPosition();
glm::quat rot = vehicle->getRotation();

// 速度向量
glm::vec3 linearVel = vehicle->getLinearVelocity();
glm::vec3 angularVel = vehicle->getAngularVelocity();

// 輪子狀態
int wheelCount = vehicle->getWheelCount();  // 通常是 4

for (int i = 0; i < wheelCount; i++) {
    bool inContact = vehicle->isWheelInContact(i);
    glm::vec3 wheelPos = vehicle->getWheelPosition(i);
    glm::vec3 contactNormal = vehicle->getWheelContactNormal(i);
    float suspensionLength = vehicle->getWheelSuspensionLength(i);
}
```

### 典型使用範例

```cpp
// 方向鍵控制車輛
void updateVehicle(VehicleController* vehicle) {
    float throttle = 0.0f;
    float steering = 0.0f;

    if (keyPressed(UP))    throttle = 1.0f;
    if (keyPressed(DOWN))  throttle = -0.5f;
    if (keyPressed(LEFT))  steering = 1.0f;
    if (keyPressed(RIGHT)) steering = -1.0f;

    vehicle->setThrottle(throttle);
    vehicle->setSteering(steering);
    vehicle->setBrake(keyPressed(SPACE) ? 1.0f : 0.0f);

    fmt::print("Speed: {:.1f} km/h\n", vehicle->getSpeedKmh());
}
```

---

## 流體物理

自定義實現，支援浮力（阿基米德原理）和阻力。

### 創建與配置

```cpp
#include "Vapor/fluid_volume.hpp"

// 創建水池（使用預設模板）
auto waterVolume = scene->createFluidVolume(
    physics.get(),
    FluidVolumeSettings::createWaterVolume(
        glm::vec3(0.0f, 2.0f, -10.0f),  // 位置
        glm::vec3(10.0f, 2.0f, 10.0f)   // 半尺寸
    )
);

// 創建油池
auto oilVolume = scene->createFluidVolume(
    physics.get(),
    FluidVolumeSettings::createOilVolume(
        glm::vec3(20.0f, 1.0f, 0.0f),
        glm::vec3(5.0f, 1.0f, 5.0f)
    )
);

// 自定義流體
FluidVolumeSettings customFluid {
    .position = glm::vec3(0, 0, 0),
    .dimensions = glm::vec3(10, 5, 10),
    .rotation = glm::identity<glm::quat>(),
    .density = 1200.0f,              // kg/m³
    .linearDragCoefficient = 0.8f,
    .angularDragCoefficient = 0.8f,
    .flowVelocity = glm::vec3(2.0f, 0, 0)  // 水流
};
```

### 設置方法

```cpp
// 修改密度（影響浮力）
waterVolume->setDensity(1000.0f);  // 水的密度
float density = waterVolume->getDensity();

// 修改阻力係數
waterVolume->setLinearDragCoefficient(0.5f);
waterVolume->setAngularDragCoefficient(0.5f);

// 設置水流速度
waterVolume->setFlowVelocity(glm::vec3(1.0f, 0.0f, 0.0f));
glm::vec3 flow = waterVolume->getFlowVelocity();

// 移動流體區域
waterVolume->setPosition(glm::vec3(5.0f, 0.0f, 0.0f));
waterVolume->setRotation(glm::angleAxis(0.5f, glm::vec3(0, 1, 0)));
```

### 查詢方法

```cpp
// 檢測物體是否在流體中
JPH::BodyID bodyID = physics->getBodyID(node->body);
bool inFluid = waterVolume->isBodyInFluid(bodyID);

// 獲取浸沒體積（m³）
float volume = waterVolume->getSubmergedVolume(bodyID);

// 獲取指定位置的流體速度
glm::vec3 flowAt = waterVolume->getFluidVelocityAt(glm::vec3(0, 1, 0));
```

### 物理原理

```cpp
// 浮力計算（自動）：F = ρ × V × g
// ρ = 流體密度
// V = 浸沒體積
// g = 重力加速度

// 阻力計算（自動）：F = -k × v²
// k = 阻力係數
// v = 相對速度（物體速度 - 水流速度）
```

---

## 物理事件

通過繼承 Node 並覆蓋虛擬方法來接收物理事件。

### 觸發器事件

```cpp
struct MyTriggerZone : public Node {
    void onTriggerEnter(Node* other) override {
        fmt::print("✓ {} entered trigger zone!\n", other->name);

        // 例如：給進入者施加向上的力
        if (other->body.valid()) {
            physics->applyCentralImpulse(other->body, glm::vec3(0, 500, 0));
        }
    }

    void onTriggerExit(Node* other) override {
        fmt::print("✗ {} left trigger zone.\n", other->name);
    }
};

// 使用
auto triggerZone = std::make_shared<MyTriggerZone>();
triggerZone->name = "JumpPad";
triggerZone->trigger = physics->createBoxTrigger(
    glm::vec3(2, 1, 2),
    glm::vec3(0, 0.5f, 0)
);
scene->addNode(triggerZone);
```

### 碰撞事件

```cpp
struct MyCollisionObject : public Node {
    void onCollisionEnter(Node* other) override {
        fmt::print("Collision started with {}\n", other->name);

        // 例如：播放音效、產生粒子等
    }

    void onCollisionExit(Node* other) override {
        fmt::print("Collision ended with {}\n", other->name);
    }
};
```

### 事件特性

- **雙向通知**：兩個物體都會收到事件
- **自動管理**：由 Physics3D::process() 自動調用
- **Frame-accurate**：在物理模擬後立即觸發

---

## Gameplay 架構

### 整體架構圖

```
┌──────────────────────────────────────────────┐
│           Application Layer                   │
│  (main.cpp / physics_demo.cpp / etc.)        │
└────────┬─────────────────────────────────────┘
         │
    ┌────┴────┬──────────────┬────────────┐
    ▼         ▼              ▼            ▼
┌─────────┐ ┌──────────┐ ┌─────────┐ ┌─────────┐
│  Scene  │ │Physics3D │ │Renderer │ │EngineCore│
└────┬────┘ └────┬─────┘ └─────────┘ └─────────┘
     │           │
     │ nodes[]   │ bodies[]
     │ fluids[]  │ triggers[]
     ▼           ▼
┌─────────────────────┐  ┌──────────────────┐
│       Node          │  │  Jolt Physics    │
│  ├─ body            │◄─┤   (Internal)     │
│  ├─ trigger         │  └──────────────────┘
│  ├─ character       │
│  └─ vehicle         │
└─────────────────────┘
```

### 核心類別關係

```cpp
class Scene {
    std::vector<std::shared_ptr<Node>> nodes;
    std::vector<std::shared_ptr<FluidVolume>> fluidVolumes;

    void update(float dt);  // 更新場景圖
};

struct Node {
    BodyHandle body;                                    // 剛體組件
    TriggerHandle trigger;                              // 觸發器組件
    std::unique_ptr<CharacterController> character;     // 角色組件
    std::unique_ptr<VehicleController> vehicle;         // 車輛組件

    // 事件回調
    virtual void onTriggerEnter(Node* other) {}
    virtual void onCollisionEnter(Node* other) {}
};

class Physics3D {
    void init(TaskScheduler* scheduler);
    void process(Scene* scene, float dt);

    // 剛體管理
    BodyHandle createBoxBody(...);
    void setLinearVelocity(BodyHandle body, glm::vec3 vel);

    // 碰撞檢測
    bool raycast(glm::vec3 from, glm::vec3 to, RaycastHit& hit);
    TriggerHandle createBoxTrigger(...);
    OverlapResult overlapSphere(...);
};
```

### 典型遊戲循環

```cpp
int main() {
    // ===== 初始化 =====
    auto engineCore = std::make_unique<EngineCore>();
    engineCore->init();

    auto renderer = createRenderer(GraphicsBackend::Vulkan);
    renderer->init(window);

    auto physics = std::make_unique<Physics3D>();
    physics->init(engineCore->getTaskScheduler());
    physics->setGravity(glm::vec3(0, -9.81f, 0));

    // ===== 場景設置 =====
    auto scene = std::make_shared<Scene>("MyGame");

    // 創建地面
    auto floor = scene->createNode("Floor");
    floor->body = physics->createBoxBody(
        glm::vec3(50, 0.5f, 50),
        glm::vec3(0, -0.5f, 0),
        glm::quat(),
        BodyMotionType::Static
    );
    physics->addBody(floor->body);

    // 創建玩家
    auto player = scene->createNode("Player");
    CharacterControllerSettings charSettings { .height = 1.8f };
    player->attachCharacterController(physics.get(), charSettings);

    // 創建水池
    auto water = scene->createFluidVolume(
        physics.get(),
        FluidVolumeSettings::createWaterVolume(
            glm::vec3(0, 2, -10),
            glm::vec3(10, 2, 10)
        )
    );

    renderer->stage(scene);

    // ===== 遊戲循環 =====
    while (!quit) {
        // 1. 輸入處理
        handleInput();

        // 2. Gameplay 邏輯
        updatePlayer(player, deltaTime);
        updateEnemies(deltaTime);

        // 3. 物理模擬
        physics->process(scene, deltaTime);

        // 4. 場景更新
        scene->update(deltaTime);

        // 5. 渲染
        renderer->draw(scene, camera);
    }

    // ===== 清理 =====
    renderer->deinit();
    physics->deinit();
    engineCore->shutdown();

    return 0;
}
```

### 資料流向

```
用戶輸入
    ↓
Gameplay 邏輯 (setThrottle, jump, applyForce, etc.)
    ↓
Physics3D::process()
    │
    ├──> 場景→物理同步 (Kinematic/Static bodies)
    ├──> 流體力施加 (浮力、阻力)
    ├──> Jolt 物理模擬 (碰撞、力、約束)
    ├──> CharacterController 更新
    ├──> VehicleController 更新
    ├──> 物理→場景同步 (Dynamic bodies)
    └──> 觸發事件處理 (onTriggerEnter, etc.)
    ↓
Scene::update() (場景圖變換更新)
    ↓
Renderer::draw() (渲染)
```

### ECS 遷移準備

當前設計已考慮未來遷移到 ECS（Entity Component System）：

```cpp
// Handle 模式（當前）
struct BodyHandle { Uint32 rid; };      // 內部 ID
struct TriggerHandle { Uint32 rid; };

// 未來 ECS 架構（範例）
struct Entity { Uint32 id; };

struct BodyComponent {
    BodyHandle physicsHandle;
    MotionType motionType;
    float mass;
};

struct CharacterComponent {
    CharacterController* controller;
};

// 系統
class PhysicsSystem {
    void update(EntityManager& entities, float dt) {
        for (auto [entity, body] : entities.view<BodyComponent>()) {
            // 更新物理
        }
    }
};
```

**設計優勢**：
- ✅ Handle 基礎：所有物理對象使用 Handle，不直接暴露指針
- ✅ 組件化：Node 已包含可選組件（body, trigger, character, vehicle）
- ✅ 系統分離：Physics3D 是獨立系統，不依賴特定場景結構
- ✅ 數據導向：內部使用 map 存儲，易於批次處理

---

## 實用範例

### 撿東西、丟東西系統

```cpp
class PickupSystem {
public:
    PickupSystem(Physics3D* physics, Camera* camera)
        : physics(physics), camera(camera) {}

    bool tryPickup() {
        // Raycast 檢測
        glm::vec3 rayStart = camera->position;
        glm::vec3 rayEnd = rayStart + camera->getForward() * 5.0f;

        RaycastHit hit;
        if (physics->raycast(rayStart, rayEnd, hit)) {
            if (hit.node && hit.node->body.valid()) {
                auto motionType = physics->getMotionType(hit.node->body);
                if (motionType == BodyMotionType::Dynamic) {
                    // 撿起
                    heldBody = hit.node->body;
                    physics->setMotionType(heldBody, BodyMotionType::Kinematic);
                    physics->setGravityFactor(heldBody, 0.0f);
                    return true;
                }
            }
        }
        return false;
    }

    void drop() {
        if (!heldBody.valid()) return;

        // 放下
        physics->setMotionType(heldBody, BodyMotionType::Dynamic);
        physics->setGravityFactor(heldBody, 1.0f);
        heldBody = BodyHandle{};
    }

    void throwObject(float force) {
        if (!heldBody.valid()) return;

        // 丟出
        physics->setMotionType(heldBody, BodyMotionType::Dynamic);
        physics->setGravityFactor(heldBody, 1.0f);

        glm::vec3 throwDirection = camera->getForward();
        physics->applyCentralImpulse(heldBody, throwDirection * force);

        heldBody = BodyHandle{};
    }

    void update(float dt) {
        if (!heldBody.valid()) return;

        // 更新抓取位置
        glm::vec3 targetPos = camera->position + camera->getForward() * 3.0f;
        glm::vec3 currentPos = physics->getPosition(heldBody);
        glm::vec3 velocity = (targetPos - currentPos) / dt;

        physics->setLinearVelocity(heldBody, velocity);
    }

private:
    Physics3D* physics;
    Camera* camera;
    BodyHandle heldBody;
};

// 使用
PickupSystem pickup(physics.get(), &camera);

if (keyPressed(E)) pickup.tryPickup();
if (keyPressed(Q)) pickup.drop();
if (mouseClicked(LEFT)) pickup.throwObject(500.0f);

pickup.update(deltaTime);
```

### 重力控制

```cpp
// 正常重力
physics->setGravity(glm::vec3(0, -9.81f, 0));

// 反轉重力
physics->setGravity(glm::vec3(0, 9.81f, 0));

// 側面重力（牆壁行走）
physics->setGravity(glm::vec3(9.81f, 0, 0));

// 無重力（太空）
physics->setGravity(glm::vec3(0, 0, 0));

// 低重力（月球）
physics->setGravity(glm::vec3(0, -1.62f, 0));
```

### 爆炸效果

```cpp
void createExplosion(Physics3D* physics, glm::vec3 center, float radius, float force) {
    OverlapResult result = physics->overlapSphere(center, radius);

    for (BodyHandle body : result.bodies) {
        glm::vec3 bodyPos = physics->getPosition(body);
        glm::vec3 direction = glm::normalize(bodyPos - center);
        float distance = glm::length(bodyPos - center);

        // 距離越近力越大
        float falloff = 1.0f - (distance / radius);
        glm::vec3 impulse = direction * force * falloff;

        physics->applyCentralImpulse(body, impulse);
    }
}

// 使用
createExplosion(physics.get(), glm::vec3(0, 1, 0), 10.0f, 1000.0f);
```

### 傳送門

```cpp
struct Portal : public Node {
    Portal* linkedPortal = nullptr;

    void onTriggerEnter(Node* other) override {
        if (!linkedPortal || !other->body.valid()) return;

        // 傳送
        glm::vec3 offset = other->getWorldPosition() - this->getWorldPosition();
        glm::vec3 newPos = linkedPortal->getWorldPosition() + offset;

        // 如果是角色控制器
        if (other->characterController) {
            other->characterController->warp(newPos);
        }
        // 如果是普通剛體
        else {
            physics->setPosition(other->body, newPos);

            // 保持速度
            glm::vec3 vel = physics->getLinearVelocity(other->body);
            physics->setLinearVelocity(other->body, vel);
        }
    }
};
```

---

## 性能優化建議

### 1. 減少 Raycast 頻率

```cpp
// ❌ 不好：每幀對所有敵人 raycast
for (auto& enemy : enemies) {
    RaycastHit hit;
    if (physics->raycast(enemy->pos, player->pos, hit)) {
        // ...
    }
}

// ✅ 好：只在需要時 raycast
if (shouldCheckLineOfSight(enemy, player)) {
    RaycastHit hit;
    if (physics->raycast(enemy->pos, player->pos, hit)) {
        // ...
    }
}
```

### 2. 使用適當的形狀

- **簡單形狀優先**：Box > Sphere > Capsule > Cylinder
- **凸包代替網格**：動態物體用 ConvexHull，不用 Mesh
- **網格只用於靜態**：地形、建築用 Mesh

### 3. 休眠物體

```cpp
// Jolt 自動管理休眠，但可以手動控制
physics->deactivateBody(body);  // 強制休眠
physics->activateBody(body);    // 喚醒
```

### 4. 批次操作

```cpp
// ✅ 好：批次施加力
std::vector<BodyHandle> bodiesToPush;
for (auto& body : allBodies) {
    if (shouldApplyForce(body)) {
        bodiesToPush.push_back(body);
    }
}

for (auto& body : bodiesToPush) {
    physics->applyCentralForce(body, force);
}
```

---

## 常見問題

### Q: 物體穿透地面？

```cpp
// 檢查：
// 1. 是否添加到場景
physics->addBody(body, true);

// 2. 是否設置正確的運動類型
physics->setMotionType(floor, BodyMotionType::Static);

// 3. 是否在物理更新前移動了物體
// 應該在 physics->process() 之後修改位置
```

### Q: 角色控制器卡在牆壁？

```cpp
// 調整 characterPadding
CharacterControllerSettings settings {
    .characterPadding = 0.05f,  // 增加填充
    .penetrationRecoverySpeed = 2.0f  // 加快恢復
};
```

### Q: 車輛翻車？

```cpp
// 降低質心，增加穩定性
VehicleSettings settings = VehicleSettings::createSedan();
settings.mass = 1800.0f;  // 增加質量

// 或在 update 中檢測並恢復
if (vehicle->isFlipped()) {
    glm::quat upright = glm::angleAxis(0.0f, glm::vec3(0, 1, 0));
    physics->setRotation(vehicle->getBodyID(), upright);
}
```

### Q: 流體浮力太強/太弱？

```cpp
// 調整密度或物體質量
waterVolume->setDensity(1200.0f);  // 增加浮力
physics->setMass(body, 150.0f);    // 增加物體質量（減少浮力效果）
```

---

## 版本資訊

- **Vapor 引擎版本**: 1.0.0
- **Jolt Physics 版本**: 5.x
- **文檔更新日期**: 2025-11-27

## 相關文檔

- [Jolt Physics 官方文檔](https://jrouwe.github.io/JoltPhysics/)
- [示例程序](../Vaporware/src/physics_demo.cpp)
- [互動演示](../Vaporware/src/interactive_physics_demo.cpp)

---

**結束** - 完整的 Vapor 物理引擎 API 參考文檔
