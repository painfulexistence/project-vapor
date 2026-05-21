# ADR-003: 測試隔離策略——不連結完整 Vapor Library

- **日期**：2026-04-29
- **狀態**：已採納

## 背景

Vapor library 的初始化需要圖形裝置（Metal/Vulkan context）、物理引擎（Jolt，需要 SSE/AVX CPU feature）、音訊裝置（miniaudio）。在 headless CI runner 上，這些依賴無法滿足。

如果測試 binary 連結完整 Vapor library，任何 `#include <Vapor/scene.hpp>` 就會拖入 Physics3D、CharacterController、VehicleController 的宣告，而這些類別的實作需要 Jolt 的動態連結——即使測試本身完全不呼叫物理功能。

## 決策

測試 binary **不連結** `Vapor` CMake target。改為：

1. **直接 `#include` 純 header 邏輯**（如 `action_manager.hpp`、`camera.hpp`）——這些 header 的實作全部在 header 內（inline），不需要 `.cpp` 連結
2. **使用 stub/partial reimplementation** 測試有外部依賴的類別（如 `MinimalNode` stub 複製 `Node` 的純邏輯部分）
3. **只連結**：`Catch2::Catch2WithMain`、`glm::glm`、`fmt::fmt`

## 理由

這個策略讓測試能在：
- 任何 CI runner（無 GPU、無音訊裝置）執行
- 毫秒級別完成（不初始化任何硬體）
- 精確測試被測邏輯，而非整個子系統

## 約束與取捨

- Stub 必須**精確複製**對應的 source 邏輯——任何偏差都會讓測試失去特徵化能力
- 當 `Node` 或其他被 stub 的類別的邏輯更新時，stub 也需要同步更新（目前靠 code review 保證）
- Integration tests（ResourceManager 真實載入、Physics3D 真實模擬）需要在有對應硬體的環境中另行建立

## 未來工作

當需要測試 Physics3D 時，可考慮：
- 在有 GPU 的 runner 上建立獨立的 integration test suite
- 或引入 Jolt 的 headless 模式（不需要 GPU，只需要 CPU）
