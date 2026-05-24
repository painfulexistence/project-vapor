# ADR-001: 引入 Catch2 v3 作為測試框架

- **日期**：2026-04-29
- **狀態**：已採納

## 背景

Project Vapor 原本沒有任何自動化測試。Demo application（Vaporware/src/main.cpp，25k LOC）是唯一的驗證手段。這使得任何重構都無法有安全網，也無法在 CI 環境中自動驗證正確性。

Phoenix Workflow Phase 2 需要為核心路徑建立特徵化測試（Characterization Tests）。

## 決策

採用 **Catch2 v3** 作為測試框架。

## 理由

| 選項 | 優點 | 缺點 |
|------|------|------|
| GoogleTest | 廣泛使用、fixture 支援強 | 需要額外安裝、CMake 整合略繁瑣 |
| Catch2 v3 | header + CMake 整合簡單、`catch_discover_tests` 自動 CTest 整合、vcpkg 直接可用 | Fixture 語法稍不同 |
| 手寫 assert | 零依賴 | 無測試報告、無 CI 整合 |

選擇 Catch2 v3 的主要原因：
1. `vcpkg.json` 只需加一行 `"catch2"` 即可安裝
2. `Catch2::Catch2WithMain` target 不需要手寫 `main()`
3. `catch_discover_tests()` 讓每個 `TEST_CASE` 自動成為獨立 CTest 條目
4. 測試 binary 可單獨執行，不依賴 CTest

## 約束

- 測試 target **不連結** Vapor library——避免觸發 Metal/Vulkan/Jolt/miniaudio 初始化
- 需要 GPU 的系統（Renderer、Physics3D、AudioManager）透過 stub / mock 測試純邏輯層
- 測試 binary 必須能在無 GPU 的 CI runner（GitHub Actions macos-15）上執行

## 影響

- `vcpkg.json` 新增 `"catch2"` 依賴
- `CMakeLists.txt` 根目錄新增 `enable_testing()` + `add_subdirectory(tests)`
- `tests/CMakeLists.txt` 定義三個獨立 test executable
- `CMakePresets.json` 新增 `ci` preset 供 GitHub Actions 使用
