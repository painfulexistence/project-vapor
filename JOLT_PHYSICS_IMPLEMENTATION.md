# Jolt 物理引擎整合實現狀態

## 📊 實現進度總覽

**已完成**: Phase 1 (100%), Phase 2.1-2.2 (67%)
**待完成**: Phase 2.3-2.4, Phase 3-5
**代碼變更**: +750 行新增，涵蓋完整的物理 API 和 Trigger 系統基礎

---

## ✅ 已完成功能 (Phase 1 & Phase 2.1-2.2)

### Phase 1: 基礎重構與 API 確立

#### 1.1 完整的物理 API ✅
**位置**: `Vapor/include/Vapor/physics_3d.hpp:92-151`

實現了完整的 Jolt 物理控制 API：

**力與力矩**:
- `applyForce()` - 在指定位置施加力（可產生力矩）
- `applyCentralForce()` - 在質心施加力
- `applyTorque()` - 施加旋轉力矩
- `applyImpulse()` - 施加衝量（瞬間改變速度）
- `applyCentralImpulse()` - 在質心施加衝量
- `applyAngularImpulse()` - 施加角衝量

**速度控制**:
- `setLinearVelocity()` / `getLinearVelocity()` - 直接設置/獲取線性速度
- `setAngularVelocity()` / `getAngularVelocity()` - 直接設置/獲取角速度

**物理屬性**:
- `setMass()` / `getMass()` - 質量管理
- `setFriction()` / `getFriction()` - 摩擦力係數
- `setRestitution()` / `getRestitution()` - 彈性係數（回彈力）
- `setLinearDamping()` / `getLinearDamping()` - 線性阻尼
- `setAngularDamping()` / `getAngularDamping()` - 角阻尼

**運動狀態**:
- `setMotionType()` / `getMotionType()` - 切換 Static/Dynamic/Kinematic
- `setGravityFactor()` / `getGravityFactor()` - 重力倍數（0.0 = 無重力）
- `activateBody()` / `deactivateBody()` / `isActive()` - 睡眠控制

**位置與旋轉**:
- `getPosition()` / `setPosition()` - 位置管理
- `getRotation()` / `setRotation()` - 旋轉管理（四元數）

**UserData 管理**:
- `setBodyUserData()` / `getBodyUserData()` - 用於 Node 關聯

**使用範例**:
```cpp
// 創建一個彈跳球
auto ball = physics->createSphereBody(0.5f, glm::vec3(0, 10, 0), glm::quat(1,0,0,0), BodyMotionType::Dynamic);
physics->addBody(ball, true);
physics->setMass(ball, 1.0f);
physics->setRestitution(ball, 0.8f);  // 高彈性
physics->applyCentralImpulse(ball, glm::vec3(5, 0, 0));  // 橫向推一把

// 創建無重力物體
physics->setGravityFactor(ball, 0.0f);
```

#### 1.2 旋轉同步修復 ✅
**位置**: `Vapor/src/physics_3d.cpp:271-327`

修復了 `Physics3D::process()` 中的旋轉同步 TODO：

**雙向同步邏輯**:
1. **Scene → Physics** (行 272-298):
   - 僅同步 `Kinematic` 和 `Static` 剛體
   - 使用 `node->getWorldRotation()` 獲取四元數
   - 調用 `bodyInterface->SetRotation()` 更新物理世界

2. **Physics → Scene** (行 308-327):
   - 僅同步 `Dynamic` 剛體
   - 使用 `bodyInterface->GetRotation()` 獲取物理結果
   - 調用 `node->setLocalRotation()` 更新場景節點

**關鍵改進**:
- 防止 Dynamic 剛體被場景覆蓋（只由物理引擎控制）
- 正確處理四元數順序 (w, x, y, z)
- 標記 `isTransformDirty` 觸發場景更新

#### 1.3 碰撞形狀擴充 ✅
**位置**: `Vapor/include/Vapor/physics_3d.hpp:76-82`

新增 4 種形狀類型：

| 形狀 | 方法 | 適用場景 |
|------|------|---------|
| **Capsule** | `createCapsuleBody()` | 角色控制器、柱狀物體 |
| **Cylinder** | `createCylinderBody()` | 圓柱、罐子、柱子 |
| **Mesh** | `createMeshBody()` | 複雜靜態環境（地形） |
| **ConvexHull** | `createConvexHullBody()` | 任意凸多邊形物體 |

**使用範例**:
```cpp
// Capsule（最適合角色碰撞體）
auto capsule = physics->createCapsuleBody(
    1.0f,  // halfHeight
    0.3f,  // radius
    glm::vec3(0, 2, 0),
    glm::quat(1,0,0,0),
    BodyMotionType::Dynamic
);

// Mesh（用於靜態地形）
std::vector<glm::vec3> vertices = {...};
std::vector<Uint32> indices = {...};
auto terrain = physics->createMeshBody(vertices, indices, glm::vec3(0,0,0), glm::quat(1,0,0,0), BodyMotionType::Static);
```

**注意事項**:
- Mesh 形狀強制使用 `Layers::NON_MOVING`（僅限靜態）
- ConvexHull 適用於動態物體，但頂點數不宜過多（<256 推薦）
- 所有形狀都使用統一的錯誤處理模式

#### 1.4 射線檢測完善 ✅
**位置**: `Vapor/src/physics_3d.cpp:421-454`

修復了兩個關鍵 TODO：

**1. Node 指針獲取**:
```cpp
Uint64 userData = bodyInterface->GetUserData(hitBodyID);
hit.node = reinterpret_cast<Node*>(userData);
```

**2. 碰撞法線計算**:
```cpp
JPH::BodyLockRead lock(physicsSystem->GetBodyLockInterface(), hitBodyID);
if (lock.Succeeded()) {
    const JPH::Body& body = lock.GetBody();
    JPH::Vec3 surfaceNormal = body.GetWorldSpaceSurfaceNormal(
        result.mSubShapeID2,
        hitPoint
    );
    hit.normal = glm::vec3(surfaceNormal.GetX(), surfaceNormal.GetY(), surfaceNormal.GetZ());
}
```

**使用範例**:
```cpp
RaycastHit hit;
if (physics->raycast(from, to, hit)) {
    std::cout << "Hit node: " << hit.node->name << "\n";
    std::cout << "Normal: " << hit.normal.x << ", " << hit.normal.y << ", " << hit.normal.z << "\n";

    // 可以根據法線應用反彈力
    glm::vec3 反彈方向 = glm::reflect(入射方向, hit.normal);
}
```

---

### Phase 2.1: 碰撞層級系統擴展 ✅

**位置**: `Vapor/src/physics_3d.cpp:59-142`

將碰撞層級從 2 層擴展到 3 層：

| 層級 | 值 | 用途 | 碰撞規則 |
|------|---|------|----------|
| `NON_MOVING` | 0 | 靜態物體（地面、牆壁） | 只與 `MOVING` 碰撞 |
| `MOVING` | 1 | 動態物體（玩家、敵人） | 與所有層級碰撞 |
| `TRIGGER` | 2 | 觸發器（檢查點、陷阱） | 只檢測 `MOVING` |

**碰撞過濾邏輯**:
```cpp
case Layers::TRIGGER:
    return inObject2 == Layers::MOVING;  // Trigger 只偵測 Dynamic
```

**BroadPhase 映射**:
```cpp
mObjectToBroadPhase[Layers::TRIGGER] = BroadPhaseLayers::TRIGGER;
```

### Phase 2.2: Trigger 創建 API ✅

**位置**:
- Header: `Vapor/include/Vapor/physics_3d.hpp:59-65, 99-108`
- Implementation: `Vapor/src/physics_3d.cpp:903-1035`

實現了完整的 Trigger 系統：

**TriggerHandle 結構**:
```cpp
struct TriggerHandle {
    Uint32 rid = UINT32_MAX;
    bool valid() const { return rid != UINT32_MAX; }
};
```

**Trigger 創建方法**:
- `createBoxTrigger()` - 盒狀觸發區
- `createSphereTrigger()` - 球形觸發區
- `createCapsuleTrigger()` - 膠囊觸發區

**Trigger 管理**:
- `removeTrigger()` - 從物理世界移除
- `destroyTrigger()` - 完全銷毀
- `setTriggerUserData()` / `getTriggerUserData()` - 關聯 Node

**關鍵特性**:
```cpp
bodySettings.mIsSensor = true;  // 不產生物理碰撞，只檢測重疊
```

**使用範例**:
```cpp
// 創建檢查點觸發器
auto checkpoint = physics->createBoxTrigger(
    glm::vec3(2, 2, 2),  // 4x4x4 盒子
    glm::vec3(10, 1, 0),
    glm::quat(1,0,0,0)
);

// 關聯到場景節點
physics->setTriggerUserData(checkpoint, reinterpret_cast<Uint64>(checkpointNode.get()));
```

---

## 🔄 待實現功能 (未來開發)

### Phase 2.3: Trigger 回呼系統 ⏳

**目標**: 實現 OnTriggerEnter/Exit 事件系統

**需要修改**:
1. 擴展 `MyContactListener` (physics_3d.cpp:144)
2. 在 `Node` 中添加虛擬回呼方法
3. 在 `Physics3D::process()` 中分發事件

**預期 API**:
```cpp
// scene.hpp
struct Node {
    virtual void onTriggerEnter(Node* other) {}
    virtual void onTriggerExit(Node* other) {}
};

// 使用範例
class CheckpointNode : public Node {
    void onTriggerEnter(Node* other) override {
        if (other->name == "Player") {
            std::cout << "Checkpoint reached!\n";
        }
    }
};
```

### Phase 2.4: 重疊測試 API ⏳

**目標**: 提供手動查詢重疊物體的功能

**預期 API**:
```cpp
struct OverlapResult {
    std::vector<Node*> nodes;
};

OverlapResult overlapSphere(const glm::vec3& center, float radius);
OverlapResult overlapBox(const glm::vec3& center, const glm::vec3& halfExtents, const glm::quat& rotation);
```

**實現提示**: 使用 `JPH::CollideShape` 和 `AllHitCollisionCollector`

### Phase 3: 角色控制器 ⏳

**目標**: 整合 `JPH::CharacterVirtual`，提供 FPS/TPS 角色運動

**需要創建**:
- `Vapor/include/Vapor/character_controller.hpp`
- `Vapor/src/character_controller.cpp`

**核心功能**:
- 膠囊體碰撞檢測
- 斜坡攀爬（最大角度限制）
- 跳躍與重力
- 推動剛體

### Phase 4: 載具物理 ⏳

**目標**: 支援多輪載具模擬

**核心組件**:
- `JPH::VehicleConstraint`
- `JPH::WheeledVehicleController`
- 輪胎懸吊系統

### Phase 5: 流體物理 ⏳

**目標**: 模擬浮力和阻力

**核心組件**:
- `FluidVolume` 類（使用 Trigger 實現水體）
- 浮力計算（阿基米德原理）
- 流體阻力

---

## 🏗️ 架構設計考量

### ECS 轉型準備

當前實現雖基於 Scene + Node + Component 架構，但已為 ECS 轉型做好準備：

**1. Handle 模式**:
```cpp
struct BodyHandle { Uint32 rid; };
struct TriggerHandle { Uint32 rid; };
```
→ 可直接轉換為 ECS Entity ID

**2. 數據與行為分離**:
- `Physics3D` 管理所有物理狀態
- `Node` 僅持有 Handle，不包含物理邏輯
→ 符合 ECS 的數據導向設計

**3. 批次操作友好**:
- `process()` 中的循環可直接轉換為系統查詢
- 所有物理狀態存儲在 `unordered_map`
→ 易於改為 ECS 的組件數組

**轉型路徑建議**:
```
當前: Node::body (BodyHandle) → Physics3D::bodies (map)
未來: Entity → PhysicsBodyComponent { BodyHandle }
         ↓
     PhysicsSystem::update(Query<Transform, PhysicsBody>)
```

### 效能優化建議

1. **形狀快取系統** (可選):
   - 雖已設計 `ShapeDesc` 結構，但未實現
   - 對於大量相同形狀的物體（如方塊遊戲）可減少記憶體

2. **批次添加**:
   - 使用 `BodyInterface::AddBodiesPrepare()` 批次添加剛體
   - 適用於場景載入時

3. **固定時間步**:
   - 當前 60Hz (1/60s)
   - 可根據需求調整為 120Hz 以提高精度

---

## 📝 使用指南

### 基本工作流程

1. **初始化物理系統**:
```cpp
Physics3D physics;
physics.init(taskScheduler);
physics.setGravity(glm::vec3(0, -9.81f, 0));
```

2. **創建剛體並關聯到節點**:
```cpp
auto node = scene->createNode("MyObject", glm::mat4(1.0f));
node->body = physics.createBoxBody(
    glm::vec3(1, 1, 1),  // halfSize
    glm::vec3(0, 5, 0),  // position
    glm::quat(1,0,0,0),  // rotation
    BodyMotionType::Dynamic
);
physics.addBody(node->body, true);  // activate immediately

// 關聯 Node 指針（用於 raycast）
physics.setBodyUserData(node->body, reinterpret_cast<Uint64>(node.get()));
```

3. **每幀更新**:
```cpp
void update(float deltaTime) {
    physics.process(scene, deltaTime);
    // 物理結果已自動同步到 scene nodes
}
```

4. **運行時操作**:
```cpp
// 爆炸效果
for (auto& node : nearbyObjects) {
    glm::vec3 direction = glm::normalize(node->getWorldPosition() - explosionCenter);
    physics.applyImpulse(node->body, direction * 1000.0f);
}

// 調整重力
physics.setGravityFactor(playerBody, 0.5f);  // 月球重力
```

---

## 🧪 測試建議

### 單元測試場景

**1. 彈跳球測試** (驗證 Restitution):
```cpp
auto ball = physics->createSphereBody(0.5f, glm::vec3(0, 10, 0), ...);
physics->setRestitution(ball, 1.0f);  // 完美彈性
// 預期：球應無限彈跳
```

**2. 旋轉同步測試**:
```cpp
auto box = physics->createBoxBody(..., BodyMotionType::Kinematic);
node->rotateAroundWorldAxis(glm::vec3(0,1,0), 0.1f);  // 每幀旋轉
// 預期：物理世界的旋轉應與場景同步
```

**3. Trigger 測試**:
```cpp
auto trigger = physics->createBoxTrigger(glm::vec3(2,2,2), glm::vec3(0,1,0));
auto ball = physics->createSphereBody(0.5f, glm::vec3(0,10,0), ...);
// 預期：球穿過觸發器時不應被阻擋
```

---

## 📦 提交資訊

**Commit**: `b98e407`
**Branch**: `claude/jolt-physics-integration-01QHt8dJWjtyWHL3o4y2bFYG`
**Files Changed**: 2 files, +750 lines
**Status**: ✅ 已推送到遠端

---

## 🚀 後續開發建議

1. **優先級 1**: 完成 Phase 2.3-2.4 (Trigger 回呼 + 重疊測試)
   - 這兩個功能對遊戲玩法至關重要
   - 實現難度低，可快速完成

2. **優先級 2**: Phase 3 角色控制器
   - 對於 FPS/TPS 遊戲必不可少
   - Jolt 提供了優秀的 CharacterVirtual 實現

3. **優先級 3**: 載具與流體物理
   - 根據具體遊戲需求決定是否實現

4. **長期**: ECS 架構遷移
   - 當前設計已為 ECS 做好準備
   - 可在不破壞現有 API 的情況下逐步遷移

---

## 📚 參考資源

- **Jolt Physics 官方文檔**: https://jrouwe.github.io/JoltPhysics/
- **Character Controller 範例**: `JoltPhysics/Samples/Tests/Character/CharacterVirtualTest.cpp`
- **Vehicle 範例**: `JoltPhysics/Samples/Tests/Vehicle/VehicleTest.cpp`
- **當前實現基於**: `JOLT_PHYSICS_INTEGRATION_ROADMAP.md` (原始路線圖)

---

**最後更新**: 2025-11-26
**實現者**: Claude (Sonnet 4.5)
**總代碼量**: ~1035 行 (physics_3d.cpp)
