# Safeguard Coverage
# 最後更新：2026-04-29
# 測試框架：Catch2 v3
# 執行測試指令：
#   cmake --preset=ninja-local-vcpkg
#   cmake --build build --config Debug
#   cd build && ctest -C Debug --output-on-failure
#
# 或直接執行測試 binary：
#   ./build/tests/Debug/test_action_system
#   ./build/tests/Debug/test_scene_transform
#   ./build/tests/Debug/test_camera

## 已覆蓋

- [x] Easing Functions (Linear/InQuad/OutQuad/InOutQuad/InCubic/OutCubic/OutBack) — `tests/action_system_safeguard_test.cpp` (8 個測試案例)
- [x] Timer (init/update/complete/progress/reset/zero-duration) — `tests/action_system_safeguard_test.cpp` (8 個測試案例)
- [x] DelayAction — `tests/action_system_safeguard_test.cpp` (2 個測試案例)
- [x] CallbackAction — `tests/action_system_safeguard_test.cpp` (2 個測試案例)
- [x] TimedCallbackAction — `tests/action_system_safeguard_test.cpp` (2 個測試案例)
- [x] UpdateAction — `tests/action_system_safeguard_test.cpp` (1 個測試案例)
- [x] TimelineAction (sequence) — `tests/action_system_safeguard_test.cpp` (2 個測試案例)
- [x] ParallelAction — `tests/action_system_safeguard_test.cpp` (3 個測試案例)
- [x] RepeatAction — `tests/action_system_safeguard_test.cpp` (2 個測試案例)
- [x] ActionManager (start/stop/tag/stopAll/null-safety) — `tests/action_system_safeguard_test.cpp` (8 個測試案例)
- [x] Node transform decomposition (position/scale/rotation/zero-scale guard) — `tests/scene_transform_safeguard_test.cpp` (5 個測試案例)
- [x] Node dirty flag (new/update/re-dirty/cache-bypass) — `tests/scene_transform_safeguard_test.cpp` (4 個測試案例)
- [x] Scene world transform propagation (root/child/3-level/parent-move) — `tests/scene_transform_safeguard_test.cpp` (5 個測試案例)
- [x] Scene findNode hierarchy — `tests/scene_transform_safeguard_test.cpp` (2 個測試案例)
- [x] Camera construction & projection modes — `tests/camera_safeguard_test.cpp` (5 個測試案例)
- [x] Camera dirty-flag caching (view/proj) — `tests/camera_safeguard_test.cpp` (4 個測試案例)
- [x] Camera::getForward — `tests/camera_safeguard_test.cpp` (1 個測試案例)
- [x] Camera frustum planes (normalization, count) — `tests/camera_safeguard_test.cpp` (2 個測試案例)
- [x] Camera::isVisible bsphere (visible/behind/far/large-radius) — `tests/camera_safeguard_test.cpp` (4 個測試案例)
- [x] Camera::isVisible AABB (visible/far/side) — `tests/camera_safeguard_test.cpp` (3 個測試案例)

**總計：68 個測試案例，3 個測試二進位檔**

## 未覆蓋（來自 FEATURES.md）

### 需要 GPU / Metal / Vulkan（不可在 unit test 中初始化）
- [ ] Forward + Tiled Light Culling
- [ ] GPU-Driven Instancing
- [ ] PBR Rendering
- [ ] MSAA / Raytraced Shadows / Bloom / DOF
- [ ] Volumetric effects / Atmosphere / Water
- [ ] 2D/3D Batch Rendering (drawQuad2D, drawLine2D...)
- [ ] Bitmap Font Rendering
- [ ] Renderer Metal / Vulkan backend

### 需要 Physics3D init（Jolt 需要完整初始化）
- [ ] Rigid Body Creation (sphere/box/capsule/cylinder/mesh/convex)
- [ ] Force & Impulse
- [ ] Trigger Volumes + callbacks
- [ ] Overlap Tests
- [ ] Raycasting
- [ ] Character Controller
- [ ] Vehicle Controller
- [ ] Shape Cache

### 需要 miniaudio init（需要音訊裝置）
- [ ] AudioManager 2D/3D playback
- [ ] Distance Models / Doppler
- [ ] Listener Control

### 需要檔案系統 / 網路（integration tests）
- [ ] ResourceManager loadImage / loadScene / loadOBJ / loadText
- [ ] ResourceManager async cache dedup

### 需要 RmlUI init
- [ ] RmlUI HUD / Subtitle / Letterbox / Chapter Title

### 純邏輯，尚未測試（下一批優先）
- [ ] InputManager action mapping / state query / history buffer
- [ ] ActionManager::UpdateForeverAction
- [ ] Scene::addMeshToNode vertex/index offset 計算
- [ ] Node::translate / rotateAroundWorldAxis
- [ ] Camera::dolly / truck / pedestal / pan / tilt / orbit
- [ ] Camera::updateAspectRatio
