# ADR-002: GitHub Actions CI Pipeline 設計

- **日期**：2026-04-29
- **狀態**：已採納

## 背景

專案目前沒有 CI/CD pipeline。Pull request 合併前沒有自動驗證，任何破壞性變更只有在手動 build + 執行 demo 時才會被發現。

## 決策

在 `.github/workflows/ci.yml` 建立兩個 job 的 CI pipeline：

1. **test** — 只編譯並執行 unit test binary（無 GPU、無 demo app）
2. **build-check** — 在 test 通過後才執行，編譯完整 Vapor library

## 理由

### 為什麼分成兩個 job？

Unit tests 比完整 build 快得多（test binary 只依賴純邏輯，沒有 Metal/Vulkan shader 編譯）。把 test 放在前面，讓邏輯錯誤能在等待 full build 之前就被發現。

### 為什麼選 macos-15？

- 引擎的主要開發平台是 macOS（Metal backend）
- GitHub Actions `macos-15` runner 使用 Apple Silicon (M1)，和開發環境一致
- Vulkan CI 需要 GPU 或 SwiftShader 等 software renderer，複雜度更高，列為後續工作

### 為什麼用 ccache？

Metal/Vulkan shader 編譯和 vcpkg 依賴編譯耗時，ccache 在 push/PR 之間大幅降低重複編譯時間。搭配 `actions/cache` 跨 run 持久化快取。

### Vaporware demo app 排除在 CI 之外

`main.cpp`（25k LOC）的 demo 需要視窗系統和 Metal context，在 headless CI runner 無法執行。CI 只編譯到 Vapor library 層。

## 約束

- CI 不執行 demo（Vaporware target 不在 CI build 內）
- 不執行 GPU-dependent 測試（Renderer、Physics3D 需要裝置）
- Vulkan CI（Linux runner）列為未來工作，待確認 SwiftShader 或 lavapipe 可用後加入

## 未來工作

- 加入 Linux runner（Vulkan backend build check）
- 加入 clang-tidy / clang-format lint job
- 加入 address sanitizer run（目前 dev preset 有 `-fsanitize=address`，CI 應也要有）

## 影響

- `.github/workflows/ci.yml` 新增
- `CMakePresets.json` 新增 `ci` preset（`inherits: global-vcpkg`，讓 CI 透過 `$VCPKG_ROOT` env var 找 toolchain）
