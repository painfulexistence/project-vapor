# Features: Project Vapor

> 分析日期：2026-04-29（最後更新：2026-05-01）
> Codebase 類型：Game Engine Library + Demo Application
> 分析來源：source code（`Vapor/include/Vapor/` 所有公開 headers + `Vaporware/src/`）

---

## Rendering System

| Feature | Description |
|---------|-------------|
| Forward + Tiled Light Culling | 主要渲染路徑；將 view frustum 切成 16×16×24 的 cluster grid，每個 cluster 只計算影響它的燈光 |
| GPU-Driven Instancing | 支援 5000+ 實例同時繪製，適用大型場景（Bistro 測試場景 2911 個實例） |
| Physically-Based Rendering | Disney BRDF 著色模型，支援 albedo、metallic、roughness、normal、emissive 貼圖 |
| MSAA 4x | 硬體多重取樣抗鋸齒 |
| Raytraced Hard Shadows | Metal-only；使用 Metal ray tracing API 產生精確硬陰影 |
| Bloom | HDR 泛光後處理效果 |
| Depth of Field | 景深模糊後處理效果（Metal：DOFCoCPass/BlurPass/CompositePass infrastructure 存在，但三個 pass 均已 comment out；⚠️ 目前停用） |
| Volumetric Fog / God Rays | 體積霧和丁達爾光束效果 |
| Volumetric Clouds | 程序式體積雲渲染 |
| Atmosphere Rendering | Rayleigh + Mie 散射大氣模型 |
| Sun / Lens Flare | 太陽鏡頭光暈效果 |
| Water Rendering | 法線貼圖水面 + 泡沫效果（Metal：WaterPass infrastructure 存在，但 pass 已 comment out；⚠️ 目前停用） |
| 2D Batch Rendering | Screen-space 四邊形、線條、矩形、圓形、三角形批次繪製 |
| 3D Batch Rendering | 世界空間帶深度測試的批次繪製 |
| Sprite / Texture Quad | 貼圖四邊形繪製（含旋轉、tint） |
| Bitmap Font Rendering | 2D screen-space 和 3D world-space 文字繪製 |
| Stylized / Realistic Shaders | 可切換的 PBR 著色風格 |
| Multi-backend | Metal（macOS）和 Vulkan（跨平台）兩套後端，共享相同 API |

---

## Physics System (Physics3D)

| Feature | Description |
|---------|-------------|
| Rigid Body Creation | 建立球形、盒形、膠囊形、圓柱形、Mesh、ConvexHull 剛體 |
| Body Lifecycle | `addBody` / `removeBody` / `destroyBody` 管理物理物體生命週期 |
| Motion Types | Static / Dynamic / Kinematic 三種運動模式 |
| Force & Impulse | `applyForce`、`applyImpulse`、`applyTorque` 等力學操作 |
| Velocity Control | `setLinearVelocity`、`setAngularVelocity` 直接控制速度 |
| Physical Properties | mass、friction、restitution、linear/angular damping、gravity factor |
| Trigger Volumes | 盒形、球形、膠囊形感應區；透過 `Node::onTriggerEnter/Exit` callback |
| Overlap Tests | `overlapSphere`、`overlapBox`、`overlapCapsule` 回傳重疊的 Node 清單 |
| Raycasting | `raycast(from, to, hit, ignoreBody)` 含命中點、法線、距離、Node 指標 |
| Gravity Control | 全域重力向量設定 |
| Body UserData | 任意 `Uint64` 可附加到 body/trigger，用於識別遊戲物件 |
| Physics Debug Draw | Wireframe 覆蓋模式視覺化碰撞形狀 |
| Fixed-step Simulation | 60Hz 固定時步 + 渲染插值 alpha |
| Shape Cache | 相同尺寸的形狀自動共用，避免重複建立 |
| Task-based Parallelism | Jolt Physics 透過 enkiTS job system 並行處理 |
| Character Controller | 膠囊形角色控制器，處理步伐、斜坡、重力 |
| Vehicle Controller | 車輛物理控制器（含輪軸、懸吊） |

---

## Audio System (AudioManager)

| Feature | Description |
|---------|-------------|
| 2D Audio Playback | `play2d(filename, loop, volume)` 播放非空間化音效 |
| 3D Spatial Audio | `play3d(filename, position/config)` 播放帶位置的空間音效 |
| Playback Control | `stop`、`pause`、`resume`（單一或全部） |
| Audio Properties | volume、pitch、loop、seek（getCurrentTime/setCurrentTime）、getDuration |
| Distance Models | None / Linear / Inverse / Exponential 四種距離衰減模型 |
| Directional Audio Cones | 內外角度 + 外部增益，模擬指向性音源 |
| Doppler Effect | 透過 velocity3D 設定產生 Doppler 頻移 |
| Listener Control | 設定聆聽者的位置、速度、朝向（camera/player 連動） |
| Master Volume | 全域音量控制 |
| Finish Callback | 音效播完時觸發 callback（在主執行緒安全呼叫） |
| Instance Pool | 最多 32 個同時播放實例 |

---

## Input System (InputManager)

| Feature | Description |
|---------|-------------|
| Action Mapping | `mapKey(scancode, InputAction)` 鍵盤按鍵到動作的映射 |
| Input State Query | `isHeld`、`isPressed`（本幀）、`isReleased`（本幀）查詢 |
| Axis / Vector Query | `getAxis(neg, pos)` 和 `getVector(4 directions)` 回傳 -1..1 的值 |
| Mouse Position | `getMousePosition()` 和 `getMouseDelta()` 取得滑鼠座標與增量 |
| Input History Buffer | 最近 32 個輸入事件的時間戳記佇列（最長保留 1 秒） |
| SDL Event Processing | `processEvent(SDL_Event)` 整合進 SDL3 事件迴圈 |
| Predefined Actions | MoveForward/Backward、Strafe、Look、Jump、Crouch、Sprint、Interact、Cancel、Hotkey1-10 |

---

## Resource Management (ResourceManager)

| Feature | Description |
|---------|-------------|
| Image Loading | `loadImage(path, Sync/Async, callback)` 載入 PNG/JPG 等圖片 |
| Scene Loading (GLTF) | `loadScene(path, optimized, Sync/Async, callback)` 載入 GLTF 場景 |
| OBJ Mesh Loading | `loadOBJ(path, mtlBasedir, Sync/Async, callback)` 載入 OBJ 網格 |
| Text File Loading | `loadText(path, Sync/Async, callback)` 載入文字檔案 |
| Thread-safe Cache | 相同路徑的資源不重複載入，跨執行緒安全存取 |
| Resource State Tracking | Unloaded / Loading / Ready / Failed 狀態追蹤 |
| Blocking Get | `Resource<T>::get()` 阻塞直到載入完成 |
| Non-blocking Try Get | `Resource<T>::tryGet()` 不阻塞，載入中回傳 nullptr |
| Completion Callbacks | 資源載入完成後觸發 callback |
| Cache Management | 分類清除（image/scene/mesh/text）或 `clearAllCaches()` |
| Load Statistics | 查詢 cache 大小、active loading task 數量 |

---

## Scene Graph (Scene / Node)

| Feature | Description |
|---------|-------------|
| Hierarchical Node Tree | 父子節點樹，世界 transform 由 local transform 遞迴計算 |
| Node Transform API | get/set LocalPosition、LocalRotation、LocalScale、LocalEulerAngles、WorldPosition 等 |
| Rotation Helpers | `rotateAroundLocalAxis`、`rotateAroundWorldAxis`、`rotate`、`translate`、`scale` |
| Node Search | `findNode(name)`、`findNodeInHierarchy(name, node)` |
| Mesh Attachment | `addMeshToNode` 將 MeshGroup 附加到節點 |
| Physics Attachment | Node 可附加 BodyHandle、TriggerHandle、CharacterController、VehicleController |
| Physics Callbacks | `onTriggerEnter/Exit`、`onCollisionEnter/Exit` 虛函式可在子類別覆寫 |
| Dirty Transform Flag | `isTransformDirty` 延遲計算世界矩陣 |
| Fluid Volume | `createFluidVolume` 建立物理流體體積 |
| Light Management | `directionalLights`、`pointLights` 清單 |
| GPU Buffer Sync | `vertices`/`indices` 平坦化後上傳至 `vertexBuffer`/`indexBuffer` |

---

## Action System (ActionManager)

| Feature | Description |
|---------|-------------|
| Easing Functions | 內建多種 easing（linear、ease-in、ease-out、ease-in-out 等） |
| Timed Actions | 依時間插值執行動畫或狀態切換 |
| Action Chaining | 序列化多個動作，前一個結束後自動觸發下一個 |
| Per-frame Update | `update(deltaTime)` 推進所有進行中的動作 |

---

## UI System (RmlUI)

| Feature | Description |
|---------|-------------|
| HTML/CSS UI Layout | 透過 RmlUi 框架載入 `.rml` 文件渲染 HUD |
| HUD with Fade | 淡入淡出的 HUD 元件 |
| Subtitle System | 含說話者名稱的字幕顯示 |
| Scroll Text | 捲動文字（電報機效果） |
| Letterbox | 電影感黑邊過渡動畫 |
| Chapter Title Cards | 帶動畫的章節標題卡 |
| Window Resize Handling | `onRmlUIResize(w, h)` 響應視窗大小變化 |
| SDL Event Integration | `processRmlUIEvent(SDL_Event)` 讓 UI 消費輸入事件 |

---

## Engine Core (EngineCore)

| Feature | Description |
|---------|-------------|
| Subsystem Orchestration | 統一管理 TaskScheduler、ResourceManager、ActionManager、InputManager、AudioManager |
| Thread Pool Init | `init(numThreads)` 建立 enkiTS worker thread pool |
| Per-frame Update | `update(deltaTime)` 推進所有子系統 |
| RmlUI Init | `initRmlUI(width, height)` 可選初始化 UI 框架 |
| Singleton Access | `EngineCore::Get()` 全域單例存取 |

---

## Camera System

| Feature | Description |
|---------|-------------|
| Perspective Projection | FOV、near/far 可設定的透視相機 |
| Orthographic Projection | 正交相機模式 |
| View Matrix Computation | 從位置、朝向計算 view matrix |
| Frustum Culling | 視錐體裁剪，剔除場景外的物件 |

---

## Developer / Debug Tools

| Feature | Description |
|---------|-------------|
| Tracy Profiler Integration | 效能剖析標記（CPU/GPU timing） |
| ImGui Debug UI | In-game debug 面板（物理參數、效能統計） |
| Physics Debug Wireframe | 即時覆蓋顯示碰撞幾何 |
| Batch Rendering Stats | `getBatch2DStats()` 查詢繪製批次數量 |
| Scene Print | `Scene::print()` 將節點樹輸出到 console |

---

## Build & Infrastructure

| Feature | Description |
|---------|-------------|
| CMake 3.20+ Build | `cmake --preset=default && cmake --build --preset=default` 一鍵建置 |
| vcpkg Dependency Management | 所有依賴透過 `vcpkg.json` 聲明與安裝，包含 Catch2 v3 |
| SPIR-V Shader Compilation | GLSL shaders 自動編譯為 SPIR-V（glslangValidator） |
| Multi-platform Graphics | macOS 選 Metal，其他平台選 Vulkan，同一套引擎 API |
| Package Config Export | 安裝後提供 CMake package config，方便外部專案使用 |
| CI/CD (GitHub Actions) | macos-15 runner，自動執行 unit tests + Vapor library build check，ccache 加速 |
| Automated Test Suite | Catch2 v3；68 個測試案例橫跨 ActionSystem、SceneTransform、Camera 三個測試二進位檔 |

---

## Graphics API Headers（新增，2026-05-01）

原始的 `graphics.hpp`（544 LOC）已重構為五個聚焦子 header，可選擇性 include：

| Header | 內容 |
|--------|------|
| `graphics_handles.hpp` | Type-safe GPU handles（`GPUHandle<Tag>` 模板，`PipelineHandle`、`BufferHandle`、`TextureHandle`、`RenderTargetHandle`） |
| `graphics_batch2d.hpp` | 2D 批次渲染結構（`BlendMode` enum、`Batch2DVertex`、`Batch2DUniforms`、`Batch2DStats`） |
| `graphics_gpu_structs.hpp` | GPU layout structs（`MaterialData`、`DirectionalLight`、`PointLight`、`CameraData`、`InstanceData`、`Cluster`、`LightCullData`、`IBLCaptureData`） |
| `graphics_mesh.hpp` | CPU-side mesh 與 material 資料（`VertexData`、`WaterVertexData`、`Mesh`、`Material`、`Image`、`AlphaMode`） |
| `graphics_effects.hpp` | 視覺特效 GPU structs（`WaterData`、`AtmosphereData`、`VolumetricFogData`、`VolumetricCloudData`、`SunFlareData`、GPU particle 系統） |

`graphics.hpp` 保留為 umbrella include，完全向後相容。

---

## Features Summary: Project Vapor

**Codebase 類型**: Game Engine Library + Demo Application (C++20)
**分析來源**: `Vapor/include/Vapor/` 所有公開 headers（34 個，2026-05-01）+ `Vaporware/src/` demo code
**Features 數量**: 80+ 個
**分組**: 是，13 個 categories（含 Graphics API Headers 拆分 + CI/CD）

完整清單：FEATURES.md
