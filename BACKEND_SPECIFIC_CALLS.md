# Backend-Specific Calls 設計文件

## 目前架構說明

目前的 RHI 設計採用抽象介面模式：

```cpp
// 抽象介面
class RHI {
public:
    virtual bool initialize(SDL_Window* window) = 0;
    virtual BufferHandle createBuffer(const BufferDesc& desc) = 0;
    // ...
};

// Vulkan 實作
class RHI_Vulkan : public RHI {
public:
    bool initialize(SDL_Window* window) override;
    // Vulkan-specific methods
    VkDevice getDevice() const { return device; }
    VkQueue getGraphicsQueue() const { return graphicsQueue; }
    // ...
};

// 使用方式
RHI* rhi = createRHIVulkan();
```

## 問題：如何呼叫 Backend-Specific 方法？

應用層通常只持有 `RHI*` 指標，因此**無法直接**呼叫 `RHI_Vulkan` 特有的方法（如 `getDevice()`）。

## 解決方案

### 方案 1：Backend Query Interface（推薦）

在 RHI 介面中加入查詢後端物件的方法，非對應後端時返回 `nullptr`。

#### 實作方式

```cpp
// rhi.hpp
class RHI {
public:
    // Backend query interface
    virtual void* getBackendDevice() const { return nullptr; }
    virtual void* getBackendQueue() const { return nullptr; }
    virtual void* getBackendCommandBuffer() const { return nullptr; }

    // Type-safe wrappers
    template<typename T>
    T* getBackendDeviceAs() const {
        return static_cast<T*>(getBackendDevice());
    }
};
```

```cpp
// rhi_vulkan.hpp
class RHI_Vulkan : public RHI {
public:
    void* getBackendDevice() const override { return (void*)device; }
    void* getBackendQueue() const override { return (void*)graphicsQueue; }
    void* getBackendCommandBuffer() const override { return (void*)currentCommandBuffer; }

private:
    VkDevice device;
    VkQueue graphicsQueue;
    VkCommandBuffer currentCommandBuffer;
};
```

#### 使用範例

```cpp
// 應用層代碼
RHI* rhi = createRHIVulkan();
rhi->initialize(window);

// 需要使用 Vulkan-specific 功能時
if (VkDevice device = rhi->getBackendDeviceAs<VkDevice>()) {
    // 現在可以直接使用 Vulkan API
    VkDescriptorPool pool;
    VkDescriptorPoolCreateInfo poolInfo{};
    // ... setup poolInfo
    vkCreateDescriptorPool(device, &poolInfo, nullptr, &pool);
}
```

#### 優點
- ✅ 保持抽象介面純淨
- ✅ 類型安全（透過 template wrapper）
- ✅ 不需要 RTTI 或 dynamic_cast
- ✅ 對不支援的後端自動返回 nullptr

#### 缺點
- ⚠️ 返回 void* 需要使用者知道正確的類型
- ⚠️ 破壞了一些抽象性

---

### 方案 2：Extension Interface Pattern

模仿 Vulkan 的擴展模式，使用命名查詢介面。

#### 實作方式

```cpp
// rhi.hpp
enum class BackendExtension {
    VulkanInterop,
    MetalInterop,
    D3D12Interop
};

class RHI {
public:
    virtual void* queryExtension(BackendExtension ext) const { return nullptr; }
};

// Vulkan 專用擴展介面
struct VulkanInteropExt {
    VkInstance instance;
    VkDevice device;
    VkPhysicalDevice physicalDevice;
    VkQueue graphicsQueue;
    VkQueue presentQueue;
    VkCommandBuffer currentCommandBuffer;
};
```

```cpp
// rhi_vulkan.cpp
void* RHI_Vulkan::queryExtension(BackendExtension ext) const {
    if (ext == BackendExtension::VulkanInterop) {
        static VulkanInteropExt interop;
        interop.instance = instance;
        interop.device = device;
        interop.physicalDevice = physicalDevice;
        interop.graphicsQueue = graphicsQueue;
        interop.presentQueue = presentQueue;
        interop.currentCommandBuffer = currentCommandBuffer;
        return &interop;
    }
    return nullptr;
}
```

#### 使用範例

```cpp
RHI* rhi = createRHIVulkan();

// 查詢 Vulkan 擴展
if (auto* vkExt = static_cast<VulkanInteropExt*>(
        rhi->queryExtension(BackendExtension::VulkanInterop))) {

    // 使用 Vulkan 物件
    VkSemaphore semaphore;
    VkSemaphoreCreateInfo semaphoreInfo{};
    semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    vkCreateSemaphore(vkExt->device, &semaphoreInfo, nullptr, &semaphore);
}
```

#### 優點
- ✅ 清晰的擴展機制
- ✅ 集中管理所有後端物件
- ✅ 易於文檔化

#### 缺點
- ⚠️ 需要維護擴展結構體
- ⚠️ 每次呼叫都需要重建結構（或使用靜態成員）

---

### 方案 3：Backend Type Query + Static Cast

使用 RTTI 或自定義類型標記來識別後端類型。

#### 實作方式

```cpp
// rhi.hpp
enum class RHIBackend {
    Vulkan,
    Metal,
    D3D12
};

class RHI {
public:
    virtual RHIBackend getBackendType() const = 0;
};
```

```cpp
// rhi_vulkan.hpp
class RHI_Vulkan : public RHI {
public:
    RHIBackend getBackendType() const override { return RHIBackend::Vulkan; }

    // Vulkan-specific public methods
    VkDevice getDevice() const { return device; }
    VkQueue getGraphicsQueue() const { return graphicsQueue; }
};
```

#### 使用範例

```cpp
RHI* rhi = createRHIVulkan();

// 檢查後端類型並轉換
if (rhi->getBackendType() == RHIBackend::Vulkan) {
    RHI_Vulkan* vkRHI = static_cast<RHI_Vulkan*>(rhi);

    // 現在可以呼叫 Vulkan-specific 方法
    VkDevice device = vkRHI->getDevice();
    VkQueue queue = vkRHI->getGraphicsQueue();

    // 使用 Vulkan API
    vkDeviceWaitIdle(device);
}
```

#### 優點
- ✅ 最簡單直接
- ✅ 完全類型安全
- ✅ 可以存取完整的 RHI_Vulkan 介面

#### 缺點
- ⚠️ 需要 include backend-specific 的標頭檔
- ⚠️ 打破抽象層
- ⚠️ 應用層代碼與後端耦合

---

### 方案 4：Capability-Based Extensions

根據功能而非後端來設計擴展介面。

#### 實作方式

```cpp
// rhi.hpp
class RHI_TimestampQuery {
public:
    virtual ~RHI_TimestampQuery() = default;
    virtual void writeTimestamp(Uint32 queryIndex) = 0;
    virtual Uint64 getTimestampResult(Uint32 queryIndex) = 0;
};

class RHI_RayTracing {
public:
    virtual ~RHI_RayTracing() = default;
    virtual void buildAccelerationStructure(/* ... */) = 0;
    virtual void traceRays(/* ... */) = 0;
};

class RHI {
public:
    // Query capabilities
    virtual RHI_TimestampQuery* getTimestampQuery() { return nullptr; }
    virtual RHI_RayTracing* getRayTracing() { return nullptr; }
};
```

```cpp
// rhi_vulkan.hpp
class RHI_Vulkan_TimestampQuery : public RHI_TimestampQuery {
    VkDevice device;
    VkQueryPool queryPool;
public:
    void writeTimestamp(Uint32 queryIndex) override;
    Uint64 getTimestampResult(Uint32 queryIndex) override;
};

class RHI_Vulkan : public RHI {
    RHI_Vulkan_TimestampQuery timestampQuery;
public:
    RHI_TimestampQuery* getTimestampQuery() override {
        return &timestampQuery;
    }
};
```

#### 使用範例

```cpp
RHI* rhi = createRHIVulkan();

// 查詢功能
if (auto* timestamps = rhi->getTimestampQuery()) {
    timestamps->writeTimestamp(0);
    Uint64 time = timestamps->getTimestampResult(0);
}
```

#### 優點
- ✅ 保持後端無關性
- ✅ 清晰的功能分界
- ✅ 易於擴展新功能

#### 缺點
- ⚠️ 需要為每個功能設計抽象介面
- ⚠️ 可能過度設計

---

## 推薦方案

### 混合方案：Capability-Based + Backend Query

對於**常見功能**使用方案 4（Capability-Based），對於**真正需要底層存取**的情況使用方案 1（Backend Query）。

```cpp
// rhi.hpp
class RHI {
public:
    // === 常見擴展功能（抽象化） ===
    virtual RHI_TimestampQuery* getTimestampQuery() { return nullptr; }
    virtual RHI_RayTracing* getRayTracing() { return nullptr; }

    // === 底層後端存取（當確實需要時） ===
    virtual void* getBackendDevice() const { return nullptr; }
    virtual void* getBackendQueue() const { return nullptr; }

    // 類型安全包裝
    template<typename T>
    T* getBackendDeviceAs() const {
        return static_cast<T*>(getBackendDevice());
    }

    template<typename T>
    T* getBackendQueueAs() const {
        return static_cast<T*>(getBackendQueue());
    }
};
```

### 使用指南

1. **優先使用 RHI 抽象介面**
   ```cpp
   rhi->createBuffer(desc);
   rhi->bindPipeline(pipeline);
   ```

2. **需要進階功能時，查詢 Capability**
   ```cpp
   if (auto* rt = rhi->getRayTracing()) {
       rt->traceRays(...);
   }
   ```

3. **只在絕對必要時存取 Backend**
   ```cpp
   // 例如：整合第三方 Vulkan 函式庫
   if (VkDevice device = rhi->getBackendDeviceAs<VkDevice>()) {
       thirdPartyLib.init(device);
   }
   ```

---

## 實作範例：ImGui 整合

這是一個實際使用 backend-specific calls 的例子：

```cpp
// 應用層代碼
void initializeImGui(SDL_Window* window, RHI* rhi) {
    ImGui_ImplSDL3_InitForVulkan(window);

    // 需要 Vulkan-specific 初始化
    if (VkDevice device = rhi->getBackendDeviceAs<VkDevice>()) {
        VkQueue queue = rhi->getBackendQueueAs<VkQueue>();

        ImGui_ImplVulkan_InitInfo initInfo{};
        initInfo.Instance = /* ... */;
        initInfo.Device = device;
        initInfo.Queue = queue;
        // ...

        ImGui_ImplVulkan_Init(&initInfo);
    }
}
```

---

## 總結

| 方案 | 適用場景 | 抽象性 | 安全性 |
|------|----------|--------|--------|
| Backend Query | 底層整合、第三方函式庫 | 中 | 中 |
| Extension Interface | 明確的擴展點 | 高 | 高 |
| Type Query + Cast | 簡單項目、快速原型 | 低 | 中 |
| Capability-Based | 跨平台功能擴展 | 高 | 高 |
| **混合方案（推薦）** | 生產環境 | 高 | 高 |

**結論：目前設計有辦法處理 backend-specific 呼叫**，建議採用混合方案來平衡抽象性和實用性。
