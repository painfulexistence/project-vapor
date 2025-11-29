# Character Controller 使用指南

## 修复内容

### 1. 相机相对移动（Camera-Relative Movement）
新增了 `moveRelativeToDirection()` 方法，支持基于相机方向的移动控制，按下前进键时角色会朝向画面正前方移动。

### 2. 跳跃修复
添加了跳跃冷却机制（200ms），防止连续跳跃导致角色一直上升的问题。

## 使用示例

```cpp
#include "Vapor/character_controller.hpp"
#include "Vapor/camera.hpp"
#include "Vapor/input_manager.hpp"

// 1. 创建角色控制器
CharacterControllerSettings settings;
settings.height = 1.8f;
settings.radius = 0.3f;
settings.mass = 70.0f;

auto characterNode = scene->createNode("Player");
characterNode->attachCharacterController(physics.get(), settings);
auto* controller = characterNode->getCharacterController();

// 设置移动速度（可选）
controller->setMaxSpeed(5.0f);  // 5 m/s

// 2. 在游戏循环中处理输入和移动

// 方式一：使用相机相对移动（推荐）
void updateCharacter(float deltaTime, const Vapor::InputState& inputState, Camera* camera) {
    auto* controller = characterNode->getCharacterController();

    // 获取输入向量
    glm::vec2 inputVector = inputState.getMovementVector(
        Vapor::InputAction::StrafeLeft,    // left
        Vapor::InputAction::StrafeRight,   // right
        Vapor::InputAction::MoveBackward,  // backward
        Vapor::InputAction::MoveForward    // forward
    );

    // 基于相机方向移动
    glm::vec3 cameraForward = camera->getForward();
    controller->moveRelativeToDirection(inputVector, cameraForward, deltaTime);

    // 处理跳跃（使用 isPressed 而非 isHeld，避免连续跳跃）
    if (inputState.isPressed(Vapor::InputAction::Jump)) {
        controller->jump(5.0f);  // 跳跃初速度 5 m/s
    }
}

// 方式二：使用世界空间移动（坦克控制）
void updateCharacterTankControls(float deltaTime, const Vapor::InputState& inputState) {
    auto* controller = characterNode->getCharacterController();

    // 直接在世界空间中移动
    glm::vec3 movement(0.0f);

    if (inputState.isHeld(Vapor::InputAction::MoveForward)) {
        movement.z -= 1.0f;  // 向北移动
    }
    if (inputState.isHeld(Vapor::InputAction::MoveBackward)) {
        movement.z += 1.0f;  // 向南移动
    }
    if (inputState.isHeld(Vapor::InputAction::StrafeLeft)) {
        movement.x -= 1.0f;  // 向西移动
    }
    if (inputState.isHeld(Vapor::InputAction::StrafeRight)) {
        movement.x += 1.0f;  // 向东移动
    }

    // 归一化并应用速度
    if (glm::length(movement) > 0.0f) {
        movement = glm::normalize(movement) * controller->getMaxSpeed();
    }

    controller->move(movement, deltaTime);
}
```

## API 参考

### 移动控制

#### `void moveRelativeToDirection(const glm::vec2& inputVector, const glm::vec3& forwardDirection, float deltaTime)`
基于给定方向（通常是相机前方）进行移动。

**参数：**
- `inputVector`: 输入向量，x=左右(-1到1)，y=前后(-1到1)
- `forwardDirection`: 参考方向向量（如相机的前方向量）
- `deltaTime`: 时间增量

**示例：**
```cpp
glm::vec2 input(0.0f, 1.0f);  // 向前移动
glm::vec3 cameraForward = camera->getForward();
controller->moveRelativeToDirection(input, cameraForward, deltaTime);
```

#### `void move(const glm::vec3& movementDirection, float deltaTime)`
在世界空间中直接移动。

**参数：**
- `movementDirection`: 世界空间中的移动方向和速度
- `deltaTime`: 时间增量

#### `void jump(float jumpSpeed)`
执行跳跃。

**参数：**
- `jumpSpeed`: 跳跃初速度（m/s），推荐值：4.0 - 6.0

**注意：**
- 只能在地面上跳跃
- 内置200ms冷却时间防止连续跳跃
- **请使用 `isPressed()` 而非 `isHeld()` 来检测跳跃输入**

### 状态查询

- `bool isOnGround()`: 检查是否在地面上
- `bool isSliding()`: 检查是否在过陡的斜坡上滑动
- `glm::vec3 getPosition()`: 获取当前位置
- `glm::vec3 getVelocity()`: 获取当前速度
- `glm::vec3 getGroundNormal()`: 获取地面法线

## 重要提示

1. **跳跃输入处理：** 必须使用 `isPressed()` 而非 `isHeld()` 检测跳跃，否则会导致连续跳跃。

   ```cpp
   // ✓ 正确
   if (inputState.isPressed(Vapor::InputAction::Jump)) {
       controller->jump(5.0f);
   }

   // ✗ 错误 - 会导致连续跳跃
   if (inputState.isHeld(Vapor::InputAction::Jump)) {
       controller->jump(5.0f);
   }
   ```

2. **移动输入处理：** 移动可以使用 `isHeld()`，因为我们希望持续移动。

3. **相机方向：** 使用 `moveRelativeToDirection()` 时，确保传入的是相机的前方向量（不是位置）。

4. **物理更新：** 角色控制器会在 `Physics3D::process()` 中自动更新，无需手动调用 `update()`。
